import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import { spawn, ChildProcessWithoutNullStreams } from 'child_process';

let previewPanel: vscode.WebviewPanel | undefined = undefined;
let serverProcess: ChildProcessWithoutNullStreams | undefined = undefined;
let outputChannel: vscode.OutputChannel;
let logChannel: vscode.OutputChannel;

// This will hold the URI of the document the preview was launched for.
let previewedDocumentUri: vscode.Uri | undefined = undefined;

let renderTimeout: NodeJS.Timeout;

export function activate(context: vscode.ExtensionContext) {
    outputChannel = vscode.window.createOutputChannel("LVGL UI Preview");
    logChannel = vscode.window.createOutputChannel("LVGL UI Preview LOG");
    logChannel.appendLine('[EXTENSION] Starting...')

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

    // --- New Robust Parser ---
    enum ParserState {
        IDLE,
        PARSING_FRAME_PAYLOAD
    }

    let parserState = ParserState.IDLE;
    let frameInfo = { x: 0, y: 0, w: 0, h: 0, payloadSize: 0 };
    let buffer = Buffer.alloc(0);

    const MAGIC_HEADER = Buffer.from("DATA:");
    const FRAME_COMMAND = Buffer.from("|FRAME|");
    const FULL_HEADER_LENGTH = MAGIC_HEADER.length + FRAME_COMMAND.length + 12; // 12 bytes for x,y,w,h,size

    function processBuffer() {
        while (true) {
            logChannel.appendLine(`[Parser] Anomaly: Loop...`)
            if (parserState === ParserState.IDLE) {
                const magicIndex = buffer.indexOf(MAGIC_HEADER);
                if (magicIndex === -1) {
                    return; // No magic header found, wait for more data
                }

                if (magicIndex > 0) {
                    //const garbage = buffer.subarray(0, magicIndex);
                    outputChannel.appendLine(`[Parser] Anomaly: Discarding ${magicIndex} bytes of unknown data before magic header.`);
                }

                // Move buffer to start of magic header
                buffer = buffer.subarray(magicIndex);

                if (buffer.length < FULL_HEADER_LENGTH) {
                    return; // Not enough data for the full header, wait for more.
                }

                const commandSlice = buffer.subarray(MAGIC_HEADER.length, MAGIC_HEADER.length + FRAME_COMMAND.length);
                if (!commandSlice.equals(FRAME_COMMAND)) {
                    outputChannel.appendLine(`[Parser] Anomaly: Found "DATA:" but unknown command "${commandSlice.toString()}". Discarding and searching again.`);
                    buffer = buffer.subarray(1); // Discard just the 'D' and search again
                    continue;
                }

                outputChannel.appendLine('[Parser] Command "|FRAME|" found. Parsing metadata.');
                const metadataOffset = MAGIC_HEADER.length + FRAME_COMMAND.length;
                frameInfo.x = buffer.readUInt16BE(metadataOffset);
                frameInfo.y = buffer.readUInt16BE(metadataOffset + 2);
                frameInfo.w = buffer.readUInt16BE(metadataOffset + 4);
                frameInfo.h = buffer.readUInt16BE(metadataOffset + 6);
                frameInfo.payloadSize = buffer.readUInt32BE(metadataOffset + 8);

                outputChannel.appendLine(`[Parser] Parsed frame metadata: { x: ${frameInfo.x}, y: ${frameInfo.y}, w: ${frameInfo.w}, h: ${frameInfo.h}, bytes: ${frameInfo.payloadSize} }`);

                buffer = buffer.subarray(FULL_HEADER_LENGTH);
                parserState = ParserState.PARSING_FRAME_PAYLOAD;
            }

            if (parserState === ParserState.PARSING_FRAME_PAYLOAD) {
                if (buffer.length < frameInfo.payloadSize) {
                    outputChannel.appendLine(`[Parser] Waiting for frame payload. Have ${buffer.length}, need ${frameInfo.payloadSize}.`);
                    return; // Need more data for payload
                }

                const frameData = buffer.subarray(0, frameInfo.payloadSize);
                outputChannel.appendLine(`[Parser] Frame complete. Read ${frameData.length} bytes (expected ${frameInfo.payloadSize}).`);

                previewPanel?.webview.postMessage({
                    command: 'updateCanvas',
                    x: frameInfo.x,
                    y: frameInfo.y,
                    width: frameInfo.w,
                    height: frameInfo.h,
                    frameBuffer: frameData.buffer // Send as ArrayBuffer
                });
                outputChannel.appendLine(`[Parser] Sent frame data to webview for rendering.`);

                buffer = buffer.subarray(frameInfo.payloadSize);
                parserState = ParserState.IDLE;
            }
        }
    }


    serverProcess.stdout.on('data', (data: Buffer) => {
        buffer = Buffer.concat([buffer, data]);
        logChannel.appendLine(`[Parser] STDOUT recv... ${data.length}`)
        processBuffer();
    });

    serverProcess.stderr.on('data', (data: Buffer) => {
        logChannel.appendLine(`[Parser] STDERR recv... ${data.length}`)
        outputChannel.appendLine(data.toString().trim());
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
        const ctx = canvas.getContext('2d', { willReadFrequently: true });
        const vscode = acquireVsCodeApi();

        let isMouseDown = false;

        // This queue will hold incoming frames.
        const frameQueue = [];
        let framePending = false;

        function renderLoop() {
            if (frameQueue.length > 0) {
                // Get the oldest frame from the queue
                const frame = frameQueue.shift();
                const { imageData, x, y, width, height } = frame;

                // The first frame (or any full-screen update) will typically have x=0, y=0
                // and its dimensions will match the full preview size. We use this to set/reset the canvas size.
                if (x === 0 && y === 0) {
                     if (canvas.width !== width || canvas.height !== height) {
                        canvas.width = width;
                        canvas.height = height;
                    }
                }

                // Draw the image data (which could be a partial frame) at the specified coordinates.
                // This is much more efficient than creating a temporary canvas.
                ctx.putImageData(imageData, x, y);
            }

            // Schedule the next frame
            requestAnimationFrame(renderLoop);
        }

        // Start the rendering loop
        renderLoop();

        window.addEventListener('message', (event) => {
            const message = event.data;
            if (message.command === 'updateCanvas') {
                const { frameBuffer, x, y, width, height } = message;

                // The received buffer is an ArrayBuffer in RGBA8888 format.
                const pixelData = new Uint8ClampedArray(frameBuffer);

                const imageData = new ImageData(pixelData, width, height);

                // Push the complete frame data into the queue for the render loop to process.
                frameQueue.push({ imageData, x, y, width, height });
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
