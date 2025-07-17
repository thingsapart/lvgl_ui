import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import { spawn, ChildProcessWithoutNullStreams } from 'child_process';

let previewPanel: vscode.WebviewPanel | undefined = undefined;
let serverProcess: ChildProcessWithoutNullStreams | undefined = undefined;
let outputChannel: vscode.OutputChannel;

// This will hold the URI of the document the preview was launched for.
let previewedDocumentUri: vscode.Uri | undefined = undefined;

let renderTimeout: NodeJS.Timeout;

export function activate(context: vscode.ExtensionContext) {
    outputChannel = vscode.window.createOutputChannel("LVGL UI Preview");

    const disposable = vscode.commands.registerCommand('lvgl-ui-generator.preview', async (uri: vscode.Uri | undefined) => {
        
        let targetUri: vscode.Uri | undefined = uri;
        
        // If the command was run from the palette, use the active editor.
        if (!targetUri) {
            const activeEditor = vscode.window.activeTextEditor;
            if (activeEditor) {
                targetUri = activeEditor.document.uri;
            }
        }

        if (!targetUri) {
            vscode.window.showInformationMessage('No active editor to preview. Please open a YAML file and click the camera icon in its title bar, or run the command from the palette with a YAML file open.');
            return;
        }

        const document = await vscode.workspace.openTextDocument(targetUri);
        if (document.languageId !== 'yaml') {
            vscode.window.showInformationMessage('The selected file is not a YAML file. This preview only works for YAML files.');
            return;
        }

        // Pin the preview to this document URI.
        previewedDocumentUri = targetUri;
        
        const editor = vscode.window.visibleTextEditors.find(e => e.document.uri.toString() === targetUri?.toString()) || await vscode.window.showTextDocument(document, vscode.ViewColumn.One);

        if (previewPanel) {
            previewPanel.reveal(vscode.ViewColumn.Beside, true);
        } else {
            previewPanel = vscode.window.createWebviewPanel(
                'lvglPreview',
                'LVGL Preview',
                { viewColumn: vscode.ViewColumn.Beside, preserveFocus: true },
                { enableScripts: true, retainContextWhenHidden: true }
            );
            setupPreviewPanel(context);
        }
        
        triggerRender(editor);
    });

    // This listener triggers a re-render ONLY when the text of the pinned document changes.
    vscode.workspace.onDidChangeTextDocument(event => {
        if (previewPanel && event.document.uri.toString() === previewedDocumentUri?.toString()) {
            const editor = vscode.window.visibleTextEditors.find(e => e.document.uri.toString() === previewedDocumentUri?.toString());
            if(editor) {
                triggerRender(editor);
            }
        }
    });

    // The onDidChangeActiveTextEditor listener has been removed to solve the issue of the
    // preview incorrectly switching to the currently focused document. The preview is now
    // "pinned" to the document it was launched with.

    context.subscriptions.push(disposable, outputChannel);
}

function setupPreviewPanel(context: vscode.ExtensionContext) {
    if (!previewPanel) return;

    previewPanel.webview.html = getWebviewContent();

    if (serverProcess) {
        serverProcess.kill();
    }
    
    const serverPath = path.join(context.extensionPath, 'bin', 'lvgl_vsc_server');
    if (!fs.existsSync(serverPath)) {
        vscode.window.showErrorMessage(`LVGL server executable not found at ${serverPath}. Please build the extension via the command palette ('Run Build Task').`);
        return;
    }

    const apiSpecPath = path.join(context.extensionPath, 'bin', 'api_spec.json');
     if (!fs.existsSync(apiSpecPath)) {
        vscode.window.showErrorMessage(`Bundled api_spec.json not found at ${apiSpecPath}. Please rebuild the extension.`);
        return;
    }

    const workspaceFolder = previewedDocumentUri ? vscode.workspace.getWorkspaceFolder(previewedDocumentUri) : undefined;
    const cwd = workspaceFolder ? workspaceFolder.uri.fsPath : undefined;
    
    serverProcess = spawn(serverPath, [apiSpecPath], { cwd });

    serverProcess.on('error', (err) => {
        outputChannel.appendLine(`Failed to start server process: ${err.message}`);
        vscode.window.showErrorMessage("Failed to start the LVGL preview server.");
    });
    
    serverProcess.stderr.on('data', (data: Buffer) => {
        outputChannel.appendLine(data.toString().trim());
    });

    // Implement a robust state-machine parser to handle the binary stream from the server.
    // This prevents screen corruption caused by misaligned frame data.
    let buffer = Buffer.alloc(0);
    let awaitingHeader = true;
    let frameInfo = { width: 0, height: 0, byteLength: 0 };

    function processBuffer() {
        while (true) { // Loop to process as much of the buffer as possible
            if (awaitingHeader) {
                const EOL = buffer.indexOf('\n');
                if (EOL === -1) {
                    return; // Need more data for header
                }
                
                const line = buffer.subarray(0, EOL).toString('utf-8');
                buffer = buffer.subarray(EOL + 1);

                const parts = line.split(' ');
                if (parts[0] === 'FRAME_DATA' && parts.length === 4) {
                    frameInfo.width = parseInt(parts[1], 10);
                    frameInfo.height = parseInt(parts[2], 10);
                    frameInfo.byteLength = parseInt(parts[3], 10);
                    awaitingHeader = false;
                    // Continue loop to process body
                } else {
                     if (line.trim().length > 0) {
                        outputChannel.appendLine(`[SERVER STDOUT UNKNOWN]: ${line}`);
                    }
                    // Continue loop to find next header
                }
            } else { // Awaiting body
                if (buffer.length >= frameInfo.byteLength) {
                    // Create a *copy* of the frame data to get a fresh ArrayBuffer of the correct size.
                    const frameData = Buffer.from(buffer.subarray(0, frameInfo.byteLength));
                    buffer = buffer.subarray(frameInfo.byteLength);

                    // Post the message. The ArrayBuffer will be serialized and sent to the webview.
                    previewPanel?.webview.postMessage({
                        command: 'updateCanvas',
                        width: frameInfo.width,
                        height: frameInfo.height,
                        // The .buffer property gets the underlying ArrayBuffer from the Node.js Buffer.
                        frameBuffer: frameData.buffer 
                    });

                    awaitingHeader = true;
                    // Continue loop to process next frame
                } else {
                    return; // Need more data for body
                }
            }
        }
    }

    serverProcess.stdout.on('data', (data: Buffer) => {
        buffer = Buffer.concat([buffer, data]);
        processBuffer();
    });

    previewPanel.onDidDispose(() => {
        serverProcess?.kill();
        serverProcess = undefined;
        previewPanel = undefined;
        previewedDocumentUri = undefined; 
    }, null, context.subscriptions);

    previewPanel.webview.onDidReceiveMessage(message => {
        if (!serverProcess) return;
        const command = {
            command: "input",
            ...message
        };
        serverProcess.stdin.write(JSON.stringify(command) + '\n');
    });
}

function triggerRender(editor: vscode.TextEditor | undefined) {
    if (!previewPanel || !serverProcess || !editor) return;

    clearTimeout(renderTimeout);
    renderTimeout = setTimeout(() => {
        const source = editor.document.getText();
        const relativePath = vscode.workspace.asRelativePath(editor.document.uri);
        
        if (previewPanel) { 
             previewPanel.title = `LVGL Preview: ${path.basename(relativePath)}`;
        }

        const config = vscode.workspace.getConfiguration('lvglPreview');
        const previewWidth = config.get<number>('width', 480);
        const previewHeight = config.get<number>('height', 320);

        const command = {
            command: 'render',
            source: source,
            width: previewWidth,
            height: previewHeight
        };
        serverProcess?.stdin.write(JSON.stringify(command) + '\n');
    }, 250);
}

function getWebviewContent(): string {
    return `<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>LVGL Preview</title>
    <style>
        body, html { margin: 0; padding: 0; width: 100%; height: 100%; display: flex; justify-content: center; align-items: center; background-color: #252526; overflow: hidden; }
        canvas { background-color: #fff; image-rendering: pixelated; image-rendering: -moz-crisp-edges; image-rendering: crisp-edges; }
    </style>
</head>
<body>
    <canvas id="preview-canvas"></canvas>
    <script>
        const canvas = document.getElementById('preview-canvas');
        const ctx = canvas.getContext('2d');
        const vscode = acquireVsCodeApi();
        
        let isMouseDown = false;
        
        // --- requestAnimationFrame Rendering Loop ---
        let latestImageData = null;
        let framePending = false;

        function renderLoop() {
            // Schedule the next frame
            requestAnimationFrame(renderLoop);
            
            // If there's no new frame, do nothing
            if (!framePending) {
                return;
            }

            // A new frame is available, draw it
            if (latestImageData) {
                if (canvas.width !== latestImageData.width) canvas.width = latestImageData.width;
                if (canvas.height !== latestImageData.height) canvas.height = latestImageData.height;
                ctx.putImageData(latestImageData, 0, 0);
            }
            
            // Mark the frame as rendered
            framePending = false;
        }

        // Start the rendering loop
        renderLoop();

        window.addEventListener('message', (event) => {
            const message = event.data;
            if (message.command === 'updateCanvas') {
                const arrayBuffer = message.frameBuffer;
                const width = message.width;
                const height = message.height;

                // The received buffer is already in RGBA8888 format.
                // We create a zero-copy view on the data.
                const pixelData = new Uint8ClampedArray(arrayBuffer);
                
                // Create an ImageData object from the pixel data.
                latestImageData = new ImageData(pixelData, width, height);

                // Signal to the render loop that a new frame is ready to be drawn.
                framePending = true;
            }
        });

        function sendMouseEvent(e, pressed) {
            const rect = canvas.getBoundingClientRect();
            const x = Math.floor(e.clientX - rect.left);
            const y = Math.floor(e.clientY - rect.top);
            vscode.postMessage({ type: 'mouse', x, y, pressed });
        }

        canvas.addEventListener('mousedown', e => { isMouseDown = true; sendMouseEvent(e, true); });
        canvas.addEventListener('mouseup', e => { isMouseDown = false; sendMouseEvent(e, false); });
        canvas.addEventListener('mousemove', e => { if (isMouseDown) sendMouseEvent(e, true); });
        canvas.addEventListener('mouseleave', e => { if(isMouseDown) { isMouseDown = false; sendMouseEvent(e, false); } });

    </script>
</body>
</html>`;
}

export function deactivate() {
    if (serverProcess) {
        serverProcess.kill();
    }
}
