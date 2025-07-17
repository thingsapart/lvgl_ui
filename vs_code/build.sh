#!/bin/bash
set -e

# --- Configuration ---
VSCODE_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
SERVER_BIN_NAME="lvgl_vsc_server"
EXTENSION_DIR="$VSCODE_DIR/extension"
SERVER_TARGET_PATH="$EXTENSION_DIR/bin/$SERVER_BIN_NAME"
PROJECT_ROOT="$VSCODE_DIR/.."

# --- Build the C Server ---
echo "--- Building VSCode LVGL Server ---"

# We need to run make from the vs_code directory so it finds all the source files correctly
cd "$VSCODE_DIR"
echo "Running make in $(pwd)..."
make -f Makefile clean
make -f Makefile

# --- Prepare Extension Directory ---
echo "--- Preparing VSCode Extension ---"
mkdir -p "$EXTENSION_DIR/bin"

# Move the server binary
mv "$SERVER_BIN_NAME" "$SERVER_TARGET_PATH"
echo "Server binary moved to $SERVER_TARGET_PATH"
chmod +x "$SERVER_TARGET_PATH"

# **NEW**: Copy the api_spec.json into the extension's binary folder
echo "Bundling api_spec.json with the extension..."
cp "$PROJECT_ROOT/api_spec.json" "$EXTENSION_DIR/bin/api_spec.json"
echo "api_spec.json copied to $EXTENSION_DIR/bin/"

# --- Build the TypeScript Extension ---
cd "$EXTENSION_DIR"

echo "Installing npm dependencies..."
npm install

echo "Compiling TypeScript..."
npm run compile

echo "--- Build Complete ---"
echo "To run the extension, open the project root folder in VSCode and press F5 (Run Extension)."
echo "To package the extension for distribution, run 'vsce package' in the '$EXTENSION_DIR' directory."
