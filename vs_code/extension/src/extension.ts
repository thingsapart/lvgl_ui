import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import { spawn, ChildProcessWithoutNullStreams } from 'child_process';

const LOGGING_ENABLED = false; // Set to true for verbose debug output

const RESOLUTIONS = [
    { name: 'Default (480x320)', width: 480, height: 320 },
    { name: 'Phone Portrait (320x480)', width: 320, height: 480 },
    { name: 'Tiny Square (240x240)', width: 240, height: 240 },
    { name: 'Small Landscape (320x240)', width: 320, height: 240 },
    { name: 'Small Portrait (240x320)', width: 240, height: 320 },
    { name: 'Large Landscape (800x480)', width: 800, height: 480 },
    { name: 'Large Portrait (480x800)', width: 480, height: 800 },
    { name: 'WXGA (1280x720)', width: 1280, height: 720 },
];
const STORAGE_KEY_RESOLUTION = 'lvglPreview.lastResolution';

let previewPanel: vscode.WebviewPanel | undefined = undefined;
let serverProcess: ChildProcessWithoutNullStreams | undefined = undefined;
let outputChannel: vscode.OutputChannel;
let logChannel: vscode.OutputChannel;
let previewedDocumentUri: vscode.Uri | undefined = undefined;
let renderTimeout: NodeJS.Timeout;

// This holds the active resolution for the current preview session.
let currentResolution = { width: 480, height: 320 };

export function activate(context: vscode.ExtensionContext) {
    outputChannel = vscode.window.createOutputChannel("LVGL UI Preview");
    logChannel = vscode.window.createOutputChannel("LVGL UI Preview LOG");
    if (LOGGING_ENABLED) {
        logChannel.appendLine('[EXTENSION] Starting...');
    }

    const disposable = vscode.commands.registerCommand('lvgl-ui-generator.preview', async (uri: vscode.Uri | undefined) => {

        let targetUri: vscode.Uri | undefined = uri;

        if (!targetUri && vscode.window.activeTextEditor) {
            targetUri = vscode.window.activeTextEditor.document.uri;
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

        previewedDocumentUri = targetUri;

        // Load the last used resolution from workspace state for this session.
        const savedRes = context.workspaceState.get<string>(STORAGE_KEY_RESOLUTION);
        if (savedRes) {
            const [width, height] = savedRes.split('x').map(Number);
            if (!isNaN(width) && !isNaN(height)) {
                currentResolution = { width, height };
            }
        } else {
            // Or use default if none is saved.
            const defaultRes = RESOLUTIONS[0];
            currentResolution = { width: defaultRes.width, height: defaultRes.height };
        }

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

        // IMPORTANT: The initial render is now triggered by the server handshake,
        // not immediately on panel creation. This ensures the server is ready.
    });

    vscode.workspace.onDidChangeTextDocument(event => {
        if (previewPanel && event.document.uri.toString() === previewedDocumentUri?.toString()) {
            const editor = vscode.window.visibleTextEditors.find(e => e.document.uri.toString() === previewedDocumentUri?.toString());
            if(editor) {
                // Re-render with the currently active resolution.
                triggerRender(editor, currentResolution.width, currentResolution.height);
            }
        }
    });

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

    const serverArgs = [apiSpecPath];
    if (LOGGING_ENABLED) {
        serverArgs.push('--log');
    }
    serverProcess = spawn(serverPath, serverArgs, { cwd });

    serverProcess.on('error', (err) => {
        outputChannel.appendLine(`Failed to start server process: ${err.message}`);
        vscode.window.showErrorMessage("Failed to start the LVGL preview server.");
    });

    // --- New Robust Parser ---
    let buffer = Buffer.alloc(0);
    const MAGIC_HEADER = Buffer.from("DATA:");
    const FRAME_COMMAND = Buffer.from("|FRAME|");
    const INIT_COMMAND = Buffer.from("|INIT |");
    const COMMAND_LENGTH = 7;
    const FULL_FRAME_HEADER_LENGTH = MAGIC_HEADER.length + COMMAND_LENGTH + 16; // + total_w,h + x,y,w,h + size
    const INIT_HEADER_LENGTH = MAGIC_HEADER.length + COMMAND_LENGTH + 4; // + w,h

    function processBuffer() {
        while (true) {
            const magicIndex = buffer.indexOf(MAGIC_HEADER);
            if (magicIndex === -1) {
                return; // Wait for more data
            }
            // Discard any data before the magic header
            if (magicIndex > 0) {
                buffer = buffer.subarray(magicIndex);
            }

            if (buffer.length < MAGIC_HEADER.length + COMMAND_LENGTH) {
                return; // Not enough data for a command, wait for more.
            }

            const commandSlice = buffer.subarray(MAGIC_HEADER.length, MAGIC_HEADER.length + COMMAND_LENGTH);

            if (commandSlice.equals(INIT_COMMAND)) {
                if (buffer.length < INIT_HEADER_LENGTH) return; // Wait for full header

                const editor = vscode.window.visibleTextEditors.find(e => e.document.uri.toString() === previewedDocumentUri?.toString());
                previewPanel?.webview.postMessage({ command: 'initialize', resolution: currentResolution, allResolutions: RESOLUTIONS });
                if (editor) {
                    triggerRender(editor, currentResolution.width, currentResolution.height);
                }
                
                buffer = buffer.subarray(INIT_HEADER_LENGTH);
                continue; // Loop again immediately to process next command in buffer

            } else if (commandSlice.equals(FRAME_COMMAND)) {
                if (buffer.length < FULL_FRAME_HEADER_LENGTH) return; // Wait for full header
                
                const metadataOffset = MAGIC_HEADER.length + COMMAND_LENGTH;
                const totalWidth = buffer.readUInt16BE(metadataOffset);
                const totalHeight = buffer.readUInt16BE(metadataOffset + 2);
                const x = buffer.readUInt16BE(metadataOffset + 4);
                const y = buffer.readUInt16BE(metadataOffset + 6);
                const w = buffer.readUInt16BE(metadataOffset + 8);
                const h = buffer.readUInt16BE(metadataOffset + 10);
                const payloadSize = buffer.readUInt32BE(metadataOffset + 12);

                if (buffer.length < FULL_FRAME_HEADER_LENGTH + payloadSize) return; // Wait for full payload

                const payload = buffer.subarray(FULL_FRAME_HEADER_LENGTH, FULL_FRAME_HEADER_LENGTH + payloadSize);

                previewPanel?.webview.postMessage({
                    command: 'updateCanvas',
                    totalWidth, totalHeight,
                    x, y, width: w, height: h,
                    // FIX: Create a clean slice of the ArrayBuffer to send.
                    // The payload Buffer is a view on a larger ArrayBuffer. payload.buffer is the *entire*
                    // original buffer. Slicing it properly ensures we only send the relevant data.
                    frameBuffer: payload.buffer.slice(payload.byteOffset, payload.byteOffset + payload.length)
                });

                if(LOGGING_ENABLED) logChannel.appendLine(`[Parser] Processed frame total_dim{${totalWidth}x${totalHeight}} rect:{ x: ${x}, y: ${y}, w: ${w}, h: ${h}}`);

                buffer = buffer.subarray(FULL_FRAME_HEADER_LENGTH + payloadSize);
                continue; // Loop again immediately

            } else {
                // Unknown command, discard the 'D' from 'DATA:' and search again
                if(LOGGING_ENABLED) logChannel.appendLine(`[Parser] Anomaly: Unknown command. Discarding byte and retrying.`);
                buffer = buffer.subarray(1);
                continue;
            }
        }
    }

    serverProcess.stdout.on('data', (data: Buffer) => {
        buffer = Buffer.concat([buffer, data]);
        if (LOGGING_ENABLED) logChannel.appendLine(`[Parser] STDOUT recv... ${data.length}`);
        processBuffer();
    });

    serverProcess.stderr.on('data', (data: Buffer) => {
        if (LOGGING_ENABLED) logChannel.appendLine(`[Parser] STDERR recv... ${data.length}`);
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

        if (message.command === 'changeResolution') {
            const { width, height } = message.resolution;
            if (LOGGING_ENABLED) logChannel.appendLine(`[Extension] Resolution change request from webview: ${width}x${height}`);

            currentResolution = { width, height };
            context.workspaceState.update(STORAGE_KEY_RESOLUTION, `${width}x${height}`);

            const editor = vscode.window.visibleTextEditors.find(e => e.document.uri.toString() === previewedDocumentUri?.toString());
            if (editor) {
                triggerRender(editor, width, height);
            }
            return;
        }

        const command = { command: "input", ...message };
        serverProcess.stdin.write(JSON.stringify(command) + '\n');
    });
}

function triggerRender(editor: vscode.TextEditor, width: number, height: number) {
    if (!previewPanel || !serverProcess) return;

    clearTimeout(renderTimeout);
    renderTimeout = setTimeout(() => {
        const source = editor.document.getText();
        const relativePath = vscode.workspace.asRelativePath(editor.document.uri);
        if (previewPanel) {
             previewPanel.title = `LVGL Preview: ${path.basename(relativePath)}`;
        }

        if (LOGGING_ENABLED) logChannel.appendLine(`[Extension] Triggering render at ${width}x${height}`);

        const command = {
            command: 'render',
            source: source,
            width: width,
            height: height
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
        body, html { margin: 0; padding: 0; width: 100%; height: 100%; display: flex; flex-direction: column; justify-content: center; align-items: center; background-color: #252526; color: #ccc; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, Cantarell, 'Open Sans', 'Helvetica Neue', sans-serif; }
        .controls { padding: 8px; background-color: #333; width: 100%; box-sizing: border-box; text-align: center; border-bottom: 1px solid #444; flex-shrink: 0; }
        #resolution-select { background: #3c3c3c; color: #f0f0f0; border: 1px solid #666; padding: 4px; border-radius: 4px; }
        .canvas-container { flex-grow: 1; display: flex; justify-content: center; align-items: center; width: 100%; overflow: auto; padding: 16px; box-sizing: border-box;}
        canvas { background-color: #fff; image-rendering: pixelated; image-rendering: -moz-crisp-edges; image-rendering: crisp-edges; box-shadow: 0 4px 12px rgba(0,0,0,0.5); flex-shrink: 0; }
    </style>
</head>
<body>
    <div class="controls">
        <label for="resolution-select" style="margin-right: 8px;">Resolution:</label>
        <select id="resolution-select"></select>
    </div>
    <div class="canvas-container">
        <canvas id="preview-canvas"></canvas>
    </div>

    <script>
        const canvas = document.getElementById('preview-canvas');
        const ctx = canvas.getContext('2d', { willReadFrequently: true });
        const vscode = acquireVsCodeApi();
        const resolutionSelect = document.getElementById('resolution-select');

        let isMouseDown = false;
        const frameQueue = [];
        
        /**
         * Sets the canvas size. Critically, this sets both the element's attributes (for the drawing buffer)
         * and its CSS style (for the displayed size) to prevent the browser from scaling the canvas.
         */
        function setCanvasSize(width, height) {
            if (canvas.width !== width || canvas.height !== height) {
                // Set the drawing buffer size
                canvas.width = width;
                canvas.height = height;

                // Set the display size
                canvas.style.width = width + 'px';
                canvas.style.height = height + 'px';
            }
        }

        function renderLoop() {
            if (frameQueue.length > 0) {
                const frame = frameQueue.shift();
                const { totalWidth, totalHeight, imageData, x, y } = frame;

                // The server now tells us the total display size with EVERY frame.
                // This is the source of truth, ensuring the canvas is always the correct size,
                // even if the initial render comes in partial chunks.
                setCanvasSize(totalWidth, totalHeight);
                
                // Draw the (potentially partial) frame data at its specified coordinates.
                ctx.putImageData(imageData, x, y);
            }
            requestAnimationFrame(renderLoop);
        }
        renderLoop();

        window.addEventListener('message', (event) => {
            const message = event.data;
            switch (message.command) {
                case 'initialize': {
                    const { resolution, allResolutions } = message;
                    
                    setCanvasSize(resolution.width, resolution.height);

                    resolutionSelect.innerHTML = '';
                    allResolutions.forEach(res => {
                        const option = document.createElement('option');
                        option.value = \`\${res.width}x\${res.height}\`;
                        option.textContent = res.name;
                        option.dataset.width = res.width;
                        option.dataset.height = res.height;
                        if (res.width === resolution.width && res.height === resolution.height) {
                            option.selected = true;
                        }
                        resolutionSelect.appendChild(option);
                    });
                    break;
                }
                case 'updateCanvas': {
                    const { totalWidth, totalHeight, frameBuffer, x, y, width, height } = message;
                    const pixelData = new Uint8ClampedArray(frameBuffer);
                    const imageData = new ImageData(pixelData, width, height);
                    frameQueue.push({ totalWidth, totalHeight, imageData, x, y });
                    break;
                }
            }
        });

        resolutionSelect.addEventListener('change', e => {
            const selectedOption = e.target.options[e.target.selectedIndex];
            const width = parseInt(selectedOption.dataset.width, 10);
            const height = parseInt(selectedOption.dataset.height, 10);
            vscode.postMessage({
                command: 'changeResolution',
                resolution: { width, height }
            });
        });

        function sendMouseEvent(e, pressed) {
            const rect = canvas.getBoundingClientRect();
            const x = Math.floor(e.clientX - rect.left);
            const y = Math.floor(e.clientY - rect.top);
            if (x < 0 || y < 0 || x >= canvas.width || y >= canvas.height) return;
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
