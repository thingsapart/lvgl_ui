#!/bin/bash

# A script to concatenate a list of files into a single formatted text block
# and copy it to the clipboard.
#
# Usage:
#   ./concat_files.sh                  - Processes files listed in data/files_default.txt.
#   ./concat_files.sh file1.c *.h      - Processes specified files, ignoring the default list.
#   ./concat_files.sh + file1.c *.h    - Adds specified files to the list from data/files_default.txt.
#   ./concat_files.sh --print          - Prints to stdout instead of copying. Can be combined with other arguments.
#
# Note: Options like --print and + must come before file arguments.
#   Example: ./concat_files.sh --print + file1.c

# --- Configuration ---
# The default list of files is read from this file, one pattern per line.
DEFAULT_FILES_CONFIG="data/files_default.txt"

# --- Script Logic ---

# Exit immediately if a command exits with a non-zero status.
set -e

# --- Find a suitable clipboard command ---
if command -v pbcopy >/dev/null 2>&1; then
  COPY_CMD="pbcopy"
elif command -v wl-copy >/dev/null 2>&1; then
  COPY_CMD="wl-copy"
elif command -v xclip >/dev/null 2>&1; then
  # xclip requires the -selection clipboard option to interact with the system clipboard
  COPY_CMD="xclip -selection clipboard"
else
  COPY_CMD=""
fi

# --- Argument Parsing and File List Generation ---
PRINT_ONLY=false
ADD_MODE=false

# Handle flags. They must appear before file arguments.
if [[ "$1" == "--print" ]]; then
  PRINT_ONLY=true
  shift # Remove --print from the list of arguments
fi

if [[ "$1" == "+" ]]; then
  ADD_MODE=true
  shift # Remove '+' from the list of arguments
fi

# Load predefined files from the configuration file.
PREDEFINED_FILES=()
if [ -f "$DEFAULT_FILES_CONFIG" ]; then
    # Use mapfile to read lines into an array. -t removes trailing newlines.
    mapfile -t PREDEFINED_FILES < "$DEFAULT_FILES_CONFIG"
else
  # The config file is required in default mode or add mode, but not replace mode.
  if [[ "$#" -eq 0 || "$ADD_MODE" = true ]]; then
    echo "Error: Default file list '$DEFAULT_FILES_CONFIG' not found." >&2
    echo "Please create it or specify file paths directly to use replace mode." >&2
    exit 1
  fi
fi

# Enable recursive globbing (e.g., **/*.js)
shopt -s globstar

# Determine the final list of files to process based on the mode.
if [ "$ADD_MODE" = true ]; then
  # ADD mode: Combine predefined files with command-line arguments.
  FILES_TO_PROCESS=("${PREDEFINED_FILES[@]}" "$@")
elif [ "$#" -gt 0 ]; then
  # REPLACE mode: Use only the files from command-line arguments.
  FILES_TO_PROCESS=("$@")
else
  # DEFAULT mode: Use the predefined list.
  FILES_TO_PROCESS=("${PREDEFINED_FILES[@]}")
fi

# The final mandatory closing line.
# FINAL_MESSAGE="\nPlease print out whole files. Don\'t give me just the section that has changed, do not omit parts with comments.\n"

FINAL_MESSAGE='
Please print out whole files, only those that you have changed. Do not give me just the section that has changed, do not omit parts with comments. Only print out the files that have been changed.

NOTE:

Format every file output the following way: start with a "!>>> {filename}" followed by a markdown code block and end with "!<<< end".

EG:

"
>> tests/test_20_something_to_test.yaml
```
# Test something here
...
```
<< end
"
'

echo "FINAL: $FINAL_MESSAGE"

# Concatenate all file contents into a single variable.
# This is done in a subshell, and its stdout is captured by the variable.
# This is more efficient than appending with `+=` in a loop.
FULL_OUTPUT=$(
  # Loop through each pattern provided in the list.
  for pattern in "${FILES_TO_PROCESS[@]}"; do
    # Expand the glob pattern. If the pattern doesn't match any files,
    # 'nullglob' makes the loop not run, preventing errors.
    shopt -s nullglob
    FILES_MATCHED=($pattern)
    shopt -u nullglob # Turn off nullglob to restore default behavior

    if [ ${#FILES_MATCHED[@]} -eq 0 ]; then
        # Print a warning to standard error if a pattern matches no files.
        echo "Warning: Pattern '$pattern' did not match any files." >&2
        continue
    fi

    for file in "${FILES_MATCHED[@]}"; do
      # Check if it's a regular file (and not a directory)
      if [ -f "$file" ]; then
        # Append the formatted block for the current file to our output
        printf "\n%s\n" "$file"
        printf '```\n'
        cat "$file"
        printf '\n```\n'
      fi
    done
  done
)

# --- Final Output Handling ---
# Check if there was any output generated.
if [ -z "$FULL_OUTPUT" ]; then
  echo "Warning: No files were processed. Nothing to do." >&2
  exit 0
fi

# Combine the generated file content with the final mandatory message.
FINAL_TEXT="${FULL_OUTPUT}${FINAL_MESSAGE}"

if [ "$PRINT_ONLY" = true ]; then
  # If --print flag was used, print the result to stdout.
  printf "%s" "$FINAL_TEXT"
else
  # Otherwise, copy to the clipboard.
  if [ -z "$COPY_CMD" ]; then
    echo "Error: No clipboard command found. Please install pbcopy, wl-copy, or xclip." >&2
    exit 1
  fi

  # Pipe the final text to the detected clipboard command.
  printf "%s" "$FINAL_TEXT" | $COPY_CMD
  echo "✅ Content copied to clipboard." >&2
fi
