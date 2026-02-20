#!/usr/bin/env python3
"""
A simple command-line tool to recursively merge multiple JSON files.

Usage:
    python json_merge.py file1.json file2.json [file3.json ...] -o merged.json

Features:
- Merges dictionaries recursively.
- For non-dictionary values (lists, strings, etc.), the value from the last
  file in the argument list takes precedence.
- If no output file is specified, the result is printed to standard output.
"""
import json
import argparse
import sys

def deep_merge(base, new):
    """
    Recursively merges two dictionaries.

    Args:
        base (dict): The base dictionary to merge into.
        new (dict): The new dictionary with values to merge.

    Returns:
        dict: The merged dictionary.
    """
    # Create a new dictionary to avoid modifying the original
    merged = base.copy()

    for key, value in new.items():
        # If key exists in both and both values are dictionaries, recurse
        if key in merged and isinstance(merged[key], dict) and isinstance(value, dict):
            merged[key] = deep_merge(merged[key], value)
        # Otherwise, the value from the `new` dictionary wins
        else:
            merged[key] = value
    return merged

def main():
    """Main function to parse arguments and perform the merge."""
    parser = argparse.ArgumentParser(
        description="A tool to recursively merge multiple JSON files.",
        epilog="Example: python json_merge.py base.json prod.json -o final.json"
    )
    parser.add_argument(
        "input_files",
        metavar="FILE",
        nargs='+',
        help="One or more input JSON files to merge."
    )
    parser.add_argument(
        "-o", "--output",
        metavar="OUTPUT_FILE",
        help="Path to the output file. If not provided, prints to stdout."
    )
    parser.add_argument(
        "--indent",
        type=int,
        default=2,
        help="Indentation level for the output JSON. Default is 2."
    )

    args = parser.parse_args()

    # Start with an empty dictionary
    merged_data = {}

    for file_path in args.input_files:
        try:
            with open(file_path, 'r') as f:
                try:
                    data = json.load(f)
                    if not isinstance(data, dict):
                        print(
                            f"Warning: Top-level structure in '{file_path}' is not a dictionary. Skipping.",
                            file=sys.stderr
                        )
                        continue
                    # Perform the deep merge
                    merged_data = deep_merge(merged_data, data)
                except json.JSONDecodeError:
                    print(f"Error: Invalid JSON in file '{file_path}'.", file=sys.stderr)
                    sys.exit(1)
        except FileNotFoundError:
            print(f"Error: File not found '{file_path}'.", file=sys.stderr)
            sys.exit(1)

    # Output the result
    if args.output:
        try:
            with open(args.output, 'w') as f:
                json.dump(merged_data, f, indent=args.indent)
            print(f"Successfully merged files into '{args.output}'.")
        except IOError as e:
            print(f"Error writing to output file '{args.output}': {e}", file=sys.stderr)
            sys.exit(1)
    else:
        # Print to standard output
        print(json.dumps(merged_data, indent=args.indent))

if __name__ == "__main__":
    main()
