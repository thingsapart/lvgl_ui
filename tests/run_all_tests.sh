#!/bin/bash

# This script runs all test suites for the lvgl_ui_generator.
# It should be run from the `tests` directory.

set -e # Exit immediately if a command exits with a non-zero status.

BASEDIR=$(dirname "$0")
cd "$BASEDIR"

echo "--- Running YAML Parser Tests ---"
(cd yaml_parser && ./run.sh)
echo ""

echo "--- Running Codegen Tests ---"
(cd codegen && ./run.sh)
echo ""

echo "--- Running IR Tests ---"
(cd ir && ./run.sh)
echo ""

echo "--- Running Error Tests ---"
(cd errors && ./run.sh)
echo ""

echo "--- Running Visual Regression Tests ---"
(cd visual && ./run.sh)
echo ""

echo "✅ All test suites completed successfully."
