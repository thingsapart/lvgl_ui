#!/usr/bin/env python3
import sys
import re

def process_block(block_lines):
    """Sorts and prints a block of trace lines."""
    if not block_lines:
        return

    header = block_lines[0]
    lines = block_lines[1:]

    actions = sorted([l for l in lines if l.strip().startswith('ACTION:')])
    states = sorted([l for l in lines if l.strip().startswith('STATE_SET:')])
    notifies = sorted([l for l in lines if l.strip().startswith('NOTIFY:')])

    print(header)
    if actions:
        print("\n".join(actions))
    if states:
        print("\n".join(states))
    if notifies:
        print("\n".join(notifies))

def main(filename):
    """Reads a trace file and prints a normalized version."""
    try:
        with open(filename, 'r') as f:
            content = f.read()
    except FileNotFoundError:
        print(f"Error: File not found at {filename}", file=sys.stderr)
        sys.exit(1)

    # Split the file into blocks based on '--- TICK' or the start header
    blocks = re.split(r'\n(?=--- (?:TICK|UI-Sim Trace))', content)

    for i, block in enumerate(blocks):
        if not block.strip():
            continue

        # Add a newline between processed blocks
        if i > 0:
            print()

        process_block(block.strip().split('\n'))

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: ./normalize_trace.py <path_to_trace_file>")
        sys.exit(1)
    main(sys.argv[1])
