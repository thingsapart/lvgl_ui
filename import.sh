#!/bin/bash

# ==============================================================================
# paste-to-files.sh (v2 - More Robust)
#
# Finds the system's paste program, reads clipboard content, and extracts
# specially formatted code blocks into files. This version is more robust
# and handles mixed line endings and introductory text correctly.
#
# A formatted block looks like this:
# !>>> path/to/your/filename.ext
# ```
# your code goes here
# ```
# !<<< end
# ==============================================================================

set -e # Exit immediately if a command exits with a non-zero status.

# --- Step 1: Find a suitable paste command ---
if command -v wl-paste &> /dev/null; then
    PASTE_CMD="wl-paste --no-newline"
elif command -v pbpaste &> /dev/null; then
    PASTE_CMD="pbpaste"
elif command -v xclip &> /dev/null; then
    PASTE_CMD="xclip -o -selection clipboard"
elif command -v xsel &> /dev/null; then
    PASTE_CMD="xsel --clipboard --output"
else
    echo "[ERROR] No paste command found." >&2
    echo "Please install 'wl-paste' (Wayland), or 'xclip'/'xsel' (X11)." >&2
    exit 1
fi

echo "[INFO] Using paste command: ${PASTE_CMD}"

# --- Step 2: Get content from clipboard ---
clipboard_content=$($PASTE_CMD || true)

if [[ -z "$clipboard_content" ]]; then
    echo "[INFO] Clipboard is empty. Nothing to do."
    exit 0
fi

# --- Step 3: Process the content and write files ---
echo "[INFO] Processing clipboard content..."

# We pipe the clipboard content through `tr` to remove carriage returns (\r),
# which can cause issues, and then into awk for processing.
echo "${clipboard_content}" | tr -d '\r' | awk '
# BEGIN block runs once before processing any input.
BEGIN {
    # Set the Record Separator to our end-of-block marker.
    RS = "!<<< end"
}

# This pattern runs for any record that CONTAINS "!>>> ".
# This is more robust than checking if it starts with it.
/.*!>>> / {
    # Make a working copy of the record ($0).
    block = $0

    # THE KEY FIX: Greedily remove all text up to and including the LAST "!>>> " marker.
    # This correctly handles introductory text before the first block.
    # For example, "intro text !>>> file.txt..." becomes " file.txt...".
    sub(/.*!>>> /, "", block)

    # Trim leading newlines and whitespace that may result from the sub() call.
    sub(/^[ \t\n]+/, "", block)

    # Find the position of the first newline character.
    first_newline_pos = index(block, "\n")

    # Extract the first line (the header).
    header_line = substr(block, 1, first_newline_pos)
    # Trim any trailing whitespace from the filename.
    sub(/\n.*/, "", header_line)

    # 2. Trim any remaining trailing whitespace (like spaces or tabs) from the end of the line.
    #    Using [[:space:]] is the most portable way to do this.
    sub(/[[:space:]]*$/, "", header_line)

    filename = header_line

    if (filename == "") {
        # This can happen if a record is just "!<<< end". We can safely ignore it.
        next
    }

    # Now, extract the code.
    # 1. Get everything *after* the first line (the header).
    code = substr(block, first_newline_pos + 1)
    # 2. Remove the starting "```" (with optional language) and the newline that follows.
    sub(/^\s*```[a-z]*\n/, "", code)
    sub(/[[:space:]]*$/, "", code)
    # 3. Remove the trailing "```" and any whitespace before it.
    sub(/\n```\s*$/, "", code)

    # Create any necessary directories for the file path.
    system("mkdir -p \"$(dirname \047" filename "\047)\"")

    # Announce what we are doing to standard error.
    printf "[OK]   Writing to file: '\''%s'\''\n", filename > "/dev/stderr"

    # Write the extracted code to the specified file.
    print code > filename

    close(filename)
}
'

echo "[INFO] Done."
