"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.activate = activate;
exports.deactivate = deactivate;
const vscode = require("vscode");
const path = require("path");
const fs = require("fs");
const child_process_1 = require("child_process");
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
const STORAGE_KEY_TRACE_SIM = 'lvglPreview.traceSimEnabled';
let previewPanel = undefined;
let serverProcess = undefined;
let outputChannel;
let logChannel;
let previewedDocumentUri = undefined;
let renderTimeout;
// This holds the active resolution for the current preview session.
let currentResolution = { width: 480, height: 320 };
// This holds the active trace setting for the current preview session.
let traceSimEnabled = false;
let apiSpec = null;
function activate(context) {
    outputChannel = vscode.window.createOutputChannel("LVGL UI Preview");
    logChannel = vscode.window.createOutputChannel("LVGL UI Preview LOG");
    if (LOGGING_ENABLED) {
        logChannel.show(true); // Show the log channel on activation if enabled
        logChannel.appendLine('[EXTENSION] Starting...');
    }
    // Load persisted settings
    traceSimEnabled = context.workspaceState.get(STORAGE_KEY_TRACE_SIM, false);
    const disposable = vscode.commands.registerCommand('lvgl-ui-generator.preview', async (uri) => {
        let targetUri = uri;
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
        // If the panel is already visible and for the current file, close it (toggle behavior).
        if (previewPanel && previewedDocumentUri?.toString() === targetUri.toString()) {
            previewPanel.dispose();
            return; // Command finished
        }
        previewedDocumentUri = targetUri;
        // Load the last used resolution from workspace state for this session.
        const savedRes = context.workspaceState.get(STORAGE_KEY_RESOLUTION);
        if (savedRes) {
            const [width, height] = savedRes.split('x').map(Number);
            if (!isNaN(width) && !isNaN(height)) {
                currentResolution = { width, height };
            }
        }
        else {
            // Or use default if none is saved.
            const defaultRes = RESOLUTIONS[0];
            currentResolution = { width: defaultRes.width, height: defaultRes.height };
        }
        if (previewPanel) {
            // If the panel exists but is for a different file, we need to restart the server
            // for the new file context. We kill the old one, and startServerProcess will be called.
            if (serverProcess) {
                serverProcess.kill();
                serverProcess = undefined;
            }
            previewPanel.reveal(vscode.ViewColumn.Beside, true);
            startServerProcess(context);
        }
        else {
            previewPanel = vscode.window.createWebviewPanel('lvglPreview', 'LVGL Preview', { viewColumn: vscode.ViewColumn.Beside, preserveFocus: true }, { enableScripts: true, retainContextWhenHidden: true });
            setupPreviewPanel(context); // This will call startServerProcess internally
        }
    });
    // Load bundled api_spec.json for semantic completions (best-effort)
    try {
        const bundled = path.join(context.extensionPath, 'bin', 'api_spec.json');
        if (fs.existsSync(bundled)) {
            const raw = fs.readFileSync(bundled, 'utf8');
            apiSpec = JSON.parse(raw);
            if (LOGGING_ENABLED)
                logChannel.appendLine('[EXT] Loaded api_spec.json for completions.');
        }
    }
    catch (e) {
        // ignore parse errors
        apiSpec = null;
    }
    vscode.workspace.onDidChangeTextDocument(event => {
        if (previewPanel && event.document.uri.toString() === previewedDocumentUri?.toString()) {
            const editor = vscode.window.visibleTextEditors.find(e => e.document.uri.toString() === previewedDocumentUri?.toString());
            if (editor) {
                // Re-render with the currently active resolution.
                triggerRender(editor, currentResolution.width, currentResolution.height, context);
            }
        }
    });
    const parseDefinedNodes = (doc) => {
        const res = [];
        const lineCount = doc.lineCount;
        for (let i = 0; i < lineCount; i++) {
            const line = doc.lineAt(i).text;
            const idMatch = line.match(/id\s*:\s*["']?(@?[A-Za-z0-9_\-]+)["']?/);
            if (idMatch) {
                const id = idMatch[1];
                const indentMatch = line.match(/^(\s*)/);
                const indent = indentMatch ? indentMatch[1] : '';
                // look backward up to 6 lines for a `type:` declaration
                let type;
                for (let b = 1; b <= 6; b++) {
                    const ln = i - b;
                    if (ln < 0)
                        break;
                    const l = doc.lineAt(ln).text;
                    const t = l.match(/type\s*:\s*([A-Za-z0-9_\-]+)/);
                    if (t) {
                        type = t[1];
                        break;
                    }
                }
                // look forward up to 6 lines for an `info:` sibling at equal or greater indent
                let info;
                for (let f = 1; f <= 6; f++) {
                    const ln = i + f;
                    if (ln >= lineCount)
                        break;
                    const l = doc.lineAt(ln).text;
                    const infoMatch = l.match(/info\s*:\s*["']?(.*?)["']?\s*$/);
                    if (infoMatch) {
                        info = infoMatch[1];
                        break;
                    }
                    // stop scanning forward if we hit a line that dedents beyond the node
                    const lIndentMatch = l.match(/^(\s*)/);
                    const lIndent = lIndentMatch ? lIndentMatch[1] : '';
                    if (lIndent.length < indent.length)
                        break;
                }
                res.push({ id, type, info, line: i, indent });
            }
        }
        // dedupe by id keeping first occurrence
        const seen = new Set();
        return res.filter(n => {
            if (seen.has(n.id))
                return false;
            seen.add(n.id);
            return true;
        });
    };
    const makeContextBlock = (indent) => {
        const ind = indent + '  ';
        return '\n' + indent + 'context:\n' +
            ind + 'axis: X\n' +
            ind + 'abs_pos:\n' +
            ind + 'wcs_pos:\n' +
            ind + 'delta_pos:\n' +
            ind + 'homed_state:\n' +
            ind + 'selected_state:\n' +
            ind + 'home_action:\n' +
            ind + 'wcs_state:\n' +
            ind + 'pos_state:\n' +
            ind + 'delta_pos_state:\n';
    };
    const completionProvider = vscode.languages.registerCompletionItemProvider('yaml', {
        provideCompletionItems(document, position) {
            const line = document.lineAt(position).text;
            const linePrefix = line.substr(0, position.character);
            const items = [];
            const defined = parseDefinedNodes(document);
            // 1) add_style completions: suggest all style ids (defined ids)
            if (/add_style\s*:\s*\[?[^\]]*$/.test(linePrefix) || /add_style\s*:/.test(line)) {
                defined.forEach(node => {
                    const displayId = node.id.startsWith('@') ? node.id : '@' + node.id;
                    const it = new vscode.CompletionItem(displayId, vscode.CompletionItemKind.Value);
                    // replace token under cursor (avoid double @)
                    const wordRange = document.getWordRangeAtPosition(position, /@?[A-Za-z0-9_\-]+/);
                    const start = wordRange ? wordRange.start : position;
                    const end = wordRange ? wordRange.end : position;
                    it.textEdit = vscode.TextEdit.replace(new vscode.Range(start, end), displayId);
                    it.detail = node.info ? (node.info.length > 80 ? node.info.substr(0, 77) + '...' : node.info) : 'Defined style id';
                    if (node.info)
                        it.documentation = new vscode.MarkdownString(node.info);
                    items.push(it);
                });
                return items;
            }
            // 1b) type: completions using api_spec (widget/object types)
            if (/\btype\s*:\s*[A-Za-z0-9_\-]*$/.test(linePrefix) && apiSpec) {
                // collect candidate types from apiSpec: keys that look like object definitions
                const types = Object.keys(apiSpec).filter(k => {
                    const v = apiSpec[k];
                    return v && typeof v === 'object' && (v.properties || v.create || v.methods || v.inherits);
                }).sort();
                types.forEach(t => {
                    const it = new vscode.CompletionItem(t, vscode.CompletionItemKind.Class);
                    it.insertText = t;
                    it.detail = 'LVGL API type';
                    items.push(it);
                });
                return items;
            }
            // 2) Completing id for use-view: only show component ids (exclude use-view) and insert context AFTER completion
            if (/\b(id)\s*:\s*@?[A-Za-z0-9_\-]*$/.test(linePrefix)) {
                // scan backwards a few lines to ensure we are inside a `type: use-view` node
                let insideUseView = false;
                for (let b = 0; b < 8; b++) {
                    const ln = position.line - b;
                    if (ln < 0)
                        break;
                    const l = document.lineAt(ln).text;
                    if (/type\s*:\s*use-view/.test(l)) {
                        insideUseView = true;
                        break;
                    }
                    // stop if we hit another top-level entry
                    if (/^\s*[-]?\s*type\s*:\s*/.test(l) && !/use-view/.test(l))
                        break;
                }
                if (insideUseView) {
                    // only include defined nodes that are not use-view (i.e., real components)
                    // and prefer those whose `type` exists in the loaded apiSpec (if available)
                    defined.filter(n => !(n.type && n.type.toLowerCase() === 'use-view'))
                        .filter(n => {
                        if (!apiSpec)
                            return true; // no spec, allow all
                        if (!n.type)
                            return true; // unknown type, allow
                        return !!apiSpec[n.type];
                    })
                        .forEach(node => {
                        const displayId = node.id.startsWith('@') ? node.id : '@' + node.id;
                        const it = new vscode.CompletionItem(displayId, vscode.CompletionItemKind.Reference);
                        const wordRange = document.getWordRangeAtPosition(position, /@?[A-Za-z0-9_\-]+/);
                        const start = wordRange ? wordRange.start : position;
                        const end = wordRange ? wordRange.end : position;
                        it.textEdit = vscode.TextEdit.replace(new vscode.Range(start, end), displayId);
                        it.detail = node.info ? (node.info.length > 80 ? node.info.substr(0, 77) + '...' : node.info) : 'Component id';
                        if (node.info)
                            it.documentation = new vscode.MarkdownString(node.info);
                        // instead of additionalTextEdits (which apply before the main edit), invoke a command AFTER completion
                        it.command = {
                            command: 'lvgl-ui.insertUseViewContext',
                            title: 'Insert use-view context',
                            arguments: [document.uri, position.line, node.indent]
                        };
                        items.push(it);
                    });
                    return items;
                }
            }
            // 3) Hints/snippets for observes: and action:
            if (/\bobserves\s*:\s*$/.test(linePrefix)) {
                const snippet = new vscode.CompletionItem('observes: example', vscode.CompletionItemKind.Snippet);
                snippet.insertText = new vscode.SnippetString('observes:\n  subject|prop: {\n    true: value,\n    default: null\n  }');
                snippet.detail = 'Example observes syntax';
                items.push(snippet);
                return items;
            }
            if (/\baction\s*:\s*$/.test(linePrefix) || /\bactions?\s*:\s*$/.test(linePrefix)) {
                const snippet = new vscode.CompletionItem('action: example', vscode.CompletionItemKind.Snippet);
                snippet.insertText = new vscode.SnippetString('action: { service|method: trigger }');
                snippet.detail = 'Example action syntax';
                items.push(snippet);
                return items;
            }
            return undefined;
        }
    }, '@', ':', ' ');
    const hoverProvider = vscode.languages.registerHoverProvider('yaml', {
        provideHover(document, position) {
            const wordRange = document.getWordRangeAtPosition(position, /[A-Za-z_\-]+/);
            if (!wordRange)
                return undefined;
            const word = document.getText(wordRange);
            if (word === 'observes' || word === 'observes:') {
                const md = new vscode.MarkdownString();
                md.appendMarkdown('`observes:` — declarative binding that maps observable states to UI changes.\n\nExample:\n');
                md.appendCodeblock('observes:\n  program|status: { disabled: { RUNNING: true, default: false } }', 'yaml');
                return new vscode.Hover(md);
            }
            if (word === 'action' || word === 'actions' || word === 'action:') {
                const md = new vscode.MarkdownString();
                md.appendMarkdown('`action:` — triggers a service or command from the UI.\n\nExample:\n');
                md.appendCodeblock('action: { jog|move|x_plus: trigger }', 'yaml');
                return new vscode.Hover(md);
            }
            return undefined;
        }
    });
    // register a command that inserts the context block AFTER the id was completed
    const insertContextCommand = vscode.commands.registerCommand('lvgl-ui.insertUseViewContext', async (uri, idLine, indent) => {
        try {
            const doc = await vscode.workspace.openTextDocument(uri);
            // check whether a `context:` already exists within the next 16 lines after the id line
            const maxLook = Math.min(doc.lineCount - 1, idLine + 16);
            for (let i = idLine + 1; i <= maxLook; i++) {
                const l = doc.lineAt(i).text;
                if (/^\s*context\s*:/.test(l))
                    return; // already present
                // if we hit a line that dedents below the id's indent, stop searching
                const lIndent = (l.match(/^(\s*)/) || ['', ''])[1];
                if (lIndent.length < indent.length)
                    break;
            }
            const editor = await vscode.window.showTextDocument(doc, { preview: false });
            const insertPos = new vscode.Position(idLine + 1, 0);
            const text = makeContextBlock(indent);
            await editor.edit(editBuilder => {
                editBuilder.insert(insertPos, text);
            });
        }
        catch (e) {
            // ignore errors silently
        }
    });
    context.subscriptions.push(disposable, outputChannel, completionProvider, hoverProvider, insertContextCommand);
}
function setupPreviewPanel(context) {
    if (!previewPanel)
        return;
    previewPanel.webview.html = getWebviewContent();
    startServerProcess(context);
    previewPanel.onDidDispose(() => {
        serverProcess?.kill();
        serverProcess = undefined;
        previewPanel = undefined;
        previewedDocumentUri = undefined;
    }, null, context.subscriptions);
    previewPanel.webview.onDidReceiveMessage(message => {
        if (message.command === 'changeResolution') {
            const { width, height } = message.resolution;
            if (LOGGING_ENABLED)
                logChannel.appendLine(`[Extension] Resolution change request from webview: ${width}x${height}`);
            currentResolution = { width, height };
            context.workspaceState.update(STORAGE_KEY_RESOLUTION, `${width}x${height}`);
            const editor = vscode.window.visibleTextEditors.find(e => e.document.uri.toString() === previewedDocumentUri?.toString());
            if (editor) {
                triggerRender(editor, width, height, context);
            }
            return;
        }
        if (message.command === 'setTrace') {
            if (LOGGING_ENABLED)
                logChannel.appendLine(`[Extension] Trace setting changed to: ${message.enabled}`);
            traceSimEnabled = message.enabled;
            context.workspaceState.update(STORAGE_KEY_TRACE_SIM, traceSimEnabled);
            // We need to restart the server for the change to take effect.
            if (serverProcess) {
                serverProcess.kill();
                serverProcess = undefined; // Immediately mark as dead. The exit handler will also run but it's fine.
            }
            // A render is needed to restart the server. The INIT from the new server will trigger it.
            startServerProcess(context);
            return;
        }
        // Forward other messages (like input) to the server if it's running
        if (!serverProcess)
            return;
        const command = { command: "input", ...message };
        const commandString = JSON.stringify(command) + '\n';
        if (LOGGING_ENABLED) {
            logChannel.appendLine(`[Extension] Sending input command string (${commandString.length} bytes): ${commandString.substring(0, 1024)}`);
        }
        serverProcess.stdin.write(commandString);
    });
}
function startServerProcess(context) {
    if (serverProcess) {
        if (LOGGING_ENABLED)
            logChannel.appendLine('[Server] Attempted to start server, but one is already running.');
        return;
    }
    if (!previewPanel) {
        if (LOGGING_ENABLED)
            logChannel.appendLine('[Server] Attempted to start server, but preview panel is closed.');
        return;
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
    if (traceSimEnabled) {
        serverArgs.push('--trace-sim-no-time');
    }
    if (LOGGING_ENABLED)
        logChannel.appendLine(`[Server] Spawning server process: ${serverPath} ${serverArgs.join(' ')}`);
    serverProcess = (0, child_process_1.spawn)(serverPath, serverArgs, { cwd });
    serverProcess.on('error', (err) => {
        if (LOGGING_ENABLED)
            logChannel.appendLine(`[Server] Failed to start server process: ${err.message}`);
        vscode.window.showErrorMessage("Failed to start the LVGL preview server.");
        serverProcess = undefined; // Ensure we know the server is dead
    });
    serverProcess.on('exit', (code, signal) => {
        if (LOGGING_ENABLED)
            logChannel.appendLine(`[Server] Process exited with code ${code}, signal ${signal}.`);
        previewPanel?.webview.postMessage({ command: 'showConsoleMessage', type: 'error', text: 'Render server has stopped. It will restart on the next change.' });
        serverProcess = undefined; // CRITICAL: Mark server as dead
    });
    let buffer = Buffer.alloc(0);
    const MAGIC_HEADER = Buffer.from("DATA:");
    const FRAME_COMMAND = Buffer.from("|FRAME|");
    const INIT_COMMAND = Buffer.from("|INIT |");
    serverProcess.stdout.on('data', (data) => {
        buffer = Buffer.concat([buffer, data]);
        if (LOGGING_ENABLED)
            logChannel.appendLine(`[Parser] STDOUT recv... ${data.length}`);
        while (true) {
            const magicIndex = buffer.indexOf(MAGIC_HEADER);
            if (magicIndex === -1)
                return;
            if (magicIndex > 0)
                buffer = buffer.subarray(magicIndex);
            if (buffer.length < 12)
                return;
            const commandSlice = buffer.subarray(5, 12);
            if (commandSlice.equals(INIT_COMMAND)) {
                if (buffer.length < 16)
                    return;
                const editor = vscode.window.visibleTextEditors.find(e => e.document.uri.toString() === previewedDocumentUri?.toString());
                previewPanel?.webview.postMessage({ command: 'initialize', resolution: currentResolution, allResolutions: RESOLUTIONS, traceSimEnabled: traceSimEnabled });
                if (editor) {
                    triggerRender(editor, currentResolution.width, currentResolution.height, context);
                }
                buffer = buffer.subarray(16);
                continue;
            }
            else if (commandSlice.equals(FRAME_COMMAND)) {
                if (buffer.length < 28)
                    return;
                const payloadSize = buffer.readUInt32BE(24);
                if (buffer.length < 28 + payloadSize)
                    return;
                const totalWidth = buffer.readUInt16BE(12);
                const totalHeight = buffer.readUInt16BE(14);
                const x = buffer.readUInt16BE(16);
                const y = buffer.readUInt16BE(18);
                const w = buffer.readUInt16BE(20);
                const h = buffer.readUInt16BE(22);
                const payload = buffer.subarray(28, 28 + payloadSize);
                previewPanel?.webview.postMessage({
                    command: 'updateCanvas',
                    totalWidth, totalHeight,
                    x, y, width: w, height: h,
                    frameBuffer: payload.buffer.slice(payload.byteOffset, payload.byteOffset + payload.length)
                });
                if (LOGGING_ENABLED)
                    logChannel.appendLine(`[Parser] Processed frame total_dim{${totalWidth}x${totalHeight}} rect:{ x: ${x}, y: ${y}, w: ${w}, h: ${h}}`);
                buffer = buffer.subarray(28 + payloadSize);
                continue;
            }
            else {
                if (LOGGING_ENABLED)
                    logChannel.appendLine(`[Parser] Anomaly: Unknown command. Discarding byte and retrying.`);
                buffer = buffer.subarray(1);
                continue;
            }
        }
    });
    let stderrBuffer = '';
    serverProcess.stderr.on('data', (data) => {
        if (LOGGING_ENABLED)
            logChannel.appendLine(`[Parser] STDERR recv... ${data.length}: ${data.toString()}`);
        stderrBuffer += data.toString();
        let eolIndex;
        while ((eolIndex = stderrBuffer.indexOf('\n')) >= 0) {
            const line = stderrBuffer.substring(0, eolIndex).trim();
            stderrBuffer = stderrBuffer.substring(eolIndex + 1);
            if (!line)
                continue;
            outputChannel.appendLine(line);
            const cleanLine = line.replace(/\x1b\[[0-9;]*m/g, '');
            const match = cleanLine.match(/^\[(ERROR|WARNING|HINT)\]\s*(.*)/);
            if (match) {
                previewPanel?.webview.postMessage({ command: 'showConsoleMessage', type: match[1].toLowerCase(), text: match[2].trim() });
            }
            else if (cleanLine.startsWith("STATE_SET:") || cleanLine.startsWith("NOTIFY:") || cleanLine.startsWith("ACTION:")) {
                previewPanel?.webview.postMessage({ command: 'showConsoleMessage', type: 'trace', text: cleanLine });
            }
        }
    });
}
function triggerRender(editor, width, height, context) {
    if (!previewPanel)
        return;
    // If the server process has died, restart it. The new process will send an
    // INIT command which will trigger a render automatically.
    if (!serverProcess) {
        if (LOGGING_ENABLED)
            logChannel.appendLine('[Extension] Server is not running. Attempting to restart...');
        startServerProcess(context);
        return; // Stop here. The restarted server will trigger the render.
    }
    previewPanel.webview.postMessage({ command: 'hideConsole' });
    clearTimeout(renderTimeout);
    renderTimeout = setTimeout(() => {
        // Re-check server process existence inside the timeout, in case it died
        if (!serverProcess) {
            if (LOGGING_ENABLED)
                logChannel.appendLine('[Extension] Server died before render timeout fired. Aborting render command.');
            return;
        }
        const source = editor.document.getText();
        const relativePath = vscode.workspace.asRelativePath(editor.document.uri);
        if (previewPanel) {
            previewPanel.title = `LVGL Preview: ${path.basename(relativePath)}`;
        }
        const command = {
            command: 'render',
            source: source,
            width: width,
            height: height
        };
        const commandString = JSON.stringify(command) + '\n';
        if (LOGGING_ENABLED) {
            logChannel.appendLine(`[Extension] Sending command string (${commandString.length} bytes): ${commandString.substring(0, 1024)}...`);
        }
        serverProcess.stdin.write(commandString);
    }, 250);
}
function getWebviewContent() {
    return `<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>LVGL Preview</title>
    <style>
        body, html { margin: 0; padding: 0; width: 100%; height: 100%; display: flex; flex-direction: column; background-color: #252526; color: #ccc; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, Cantarell, 'Open Sans', 'Helvetica Neue', sans-serif; overflow: hidden; }
        .main-content { flex-grow: 1; display: flex; flex-direction: column; min-height: 0; /* Flexbox fix for overflow */ }
        .controls { padding: 8px; background-color: #333; width: 100%; box-sizing: border-box; display: flex; align-items: center; justify-content: center; border-bottom: 1px solid #444; flex-shrink: 0; gap: 16px; }
        #resolution-select { background: #3c3c3c; color: #f0f0f0; border: 1px solid #666; padding: 4px; border-radius: 4px; }
        .canvas-container { flex-grow: 1; display: flex; justify-content: center; align-items: center; width: 100%; overflow: auto; padding: 16px; box-sizing: border-box;}
        canvas { background-color: #fff; image-rendering: pixelated; image-rendering: -moz-crisp-edges; image-rendering: crisp-edges; box-shadow: 0 4px 12px rgba(0,0,0,0.5); flex-shrink: 0; }

        /* Console Styles */
        .console-container {
            height: 200px;
            background-color: #1e1e1e;
            border-top: 1px solid #444;
            display: flex;
            flex-direction: column;
            font-family: 'Courier New', Courier, monospace;
            font-size: 13px;
            flex-shrink: 0;
        }
        .console-header {
            background-color: #333;
            padding: 2px 8px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            flex-shrink: 0;
            user-select: none;
        }
        .console-header button {
            background: none;
            border: none;
            color: #ccc;
            font-size: 18px;
            line-height: 1;
            padding: 2px 6px;
            cursor: pointer;
        }
        .console-header button:hover { background-color: #555; }
        #console-output {
            flex-grow: 1;
            overflow-y: auto;
            padding: 8px;
            margin: 0;
            white-space: pre-wrap;
            word-wrap: break-word;
        }
        .console-msg { line-height: 1.4; }
        .console-msg.error { color: #f48771; }
        .console-msg.warning { color: #f1d75c; }
        .console-msg.hint { color: #6fbcf1; }
        .console-msg.trace { color: #999; }
    </style>
</head>
<body>
    <div class="main-content">
        <div class="controls">
            <div>
                <label for="resolution-select" style="margin-right: 8px;">Resolution:</label>
                <select id="resolution-select"></select>
            </div>
            <div>
                 <label><input type="checkbox" id="trace-sim-checkbox"> Trace UI simulation</label>
            </div>
        </div>
        <div class="canvas-container">
            <canvas id="preview-canvas"></canvas>
        </div>
    </div>

    <div id="console" class="console-container" style="display: none;">
        <div class="console-header">
            <span>Console</span>
            <button id="console-close-btn">×</button>
        </div>
        <pre id="console-output"></pre>
    </div>

    <script>
        const canvas = document.getElementById('preview-canvas');
        const ctx = canvas.getContext('2d', { willReadFrequently: true });
        const vscode = acquireVsCodeApi();
        const resolutionSelect = document.getElementById('resolution-select');
        const traceSimCheckbox = document.getElementById('trace-sim-checkbox');
        const consoleContainer = document.getElementById('console');
        const consoleOutput = document.getElementById('console-output');
        const consoleCloseBtn = document.getElementById('console-close-btn');

        let isMouseDown = false;
        const frameQueue = [];

        consoleCloseBtn.addEventListener('click', () => {
            consoleContainer.style.display = 'none';
        });

        function setCanvasSize(width, height) {
            if (canvas.width !== width || canvas.height !== height) {
                canvas.width = width;
                canvas.height = height;
                canvas.style.width = width + 'px';
                canvas.style.height = height + 'px';
            }
        }

        function renderLoop() {
            if (frameQueue.length > 0) {
                const frame = frameQueue.shift();
                const { totalWidth, totalHeight, imageData, x, y } = frame;
                setCanvasSize(totalWidth, totalHeight);
                ctx.putImageData(imageData, x, y);
            }
            requestAnimationFrame(renderLoop);
        }
        renderLoop();

        window.addEventListener('message', (event) => {
            const message = event.data;
            switch (message.command) {
                case 'initialize': {
                    const { resolution, allResolutions, traceSimEnabled } = message;

                    setCanvasSize(resolution.width, resolution.height);

                    // Set trace checkbox state
                    traceSimCheckbox.checked = !!traceSimEnabled;

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
                case 'hideConsole': {
                    consoleContainer.style.display = 'none';
                    consoleOutput.innerHTML = ''; // Clear previous messages
                    break;
                }
                case 'showConsoleMessage': {
                    const { type, text } = message;
                    const msgElement = document.createElement('div');
                    msgElement.className = \`console-msg \${type}\`;

                    if (type === 'trace') {
                        msgElement.textContent = text;
                    } else {
                        const prefix = \`[\${type.toUpperCase()}]\`;
                        const prefixSpan = document.createElement('span');
                        prefixSpan.style.fontWeight = 'bold';
                        prefixSpan.textContent = prefix.padEnd(10, ' ');
                        msgElement.appendChild(prefixSpan);
                        msgElement.appendChild(document.createTextNode(text));
                    }
                    consoleOutput.appendChild(msgElement);

                    // Show console if it's hidden and scroll to the new message
                    consoleContainer.style.display = 'flex';
                    consoleOutput.scrollTop = consoleOutput.scrollHeight;
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

        traceSimCheckbox.addEventListener('change', e => {
            vscode.postMessage({
                command: 'setTrace',
                enabled: e.target.checked
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
function deactivate() {
    if (serverProcess) {
        serverProcess.kill();
    }
}
//# sourceMappingURL=extension.js.map