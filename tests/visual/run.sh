#!/bin/bash

# Visual Regression Test runner.
# Runs the lvgl_render backend with a special flag to take a screenshot,
# then compares it against a .expected image using ImageMagick.
#
# Usage:
#   ./run.sh          - Run all tests and compare against .expected files.
#   ./run.sh --update - Regenerate all .expected files with the current output.

set -e

GREEN="\033[0;32m"
RED="\033[0;31m"
YELLOW="\033[0;33m"
NC="\033[0m"

# Check for dependencies
if ! command -v compare &> /dev/null; then
    echo -e "${RED}Error: ImageMagick's 'compare' utility not found. Please install it.${NC}"
    echo "  On Debian/Ubuntu: sudo apt-get install imagemagick"
    echo "  On macOS: brew install imagemagick"
    exit 1
fi

GENERATOR_EXE="../../lvgl_ui_generator"
API_SPEC_PATH="../../api_spec.json"
TEST_DIR=$(dirname "$0")

UPDATE_MODE=0
if [ "$1" = "--update" ]; then
    UPDATE_MODE=1
    echo -e "${YELLOW}--- UPDATE MODE ENABLED: Visual .expected files will be regenerated. ---${NC}"
fi

failed_tests=0
test_count=0

if [ ! -x "$GENERATOR_EXE" ]; then
    echo -e "${RED}Error: Generator executable not found at '$GENERATOR_EXE'. Please build it first.${NC}"
    exit 1
fi

for test_yaml in "$TEST_DIR"/*.yaml; do
    test_count=$((test_count + 1))
    test_name=$(basename "${test_yaml}" .yaml)
    expected_file="${TEST_DIR}/${test_name}.png.expected"
    actual_file="/tmp/${test_name}.png.actual"
    diff_file="/tmp/${test_name}.png.diff"

    if [ "$UPDATE_MODE" -eq 1 ]; then
        echo "[UPDATING] Visual: ${test_name}.png.expected"
        "$GENERATOR_EXE" "$API_SPEC_PATH" "$test_yaml" --codegen lvgl_render --screenshot-and-exit "$expected_file" > /dev/null 2>&1
        echo -e "  ${GREEN}Expected file updated: ${expected_file}${NC}"
        continue
    elif [ ! -f "$expected_file" ]; then
        echo -e "[${YELLOW}SKIP${NC}] Visual: ${test_name} (No .png.expected file. Run with --update to create.)"
        continue
    fi
    
    printf "[RUNNING] Visual: %-30s" "${test_name}"

    # Generate the screenshot
    "$GENERATOR_EXE" "$API_SPEC_PATH" "$test_yaml" --codegen lvgl_render --screenshot-and-exit "$actual_file" > /dev/null 2>&1
    
    # Compare the images
    if compare -metric AE -fuzz 1% "$expected_file" "$actual_file" "$diff_file" > /dev/null 2>&1; then
        # AE (Absolute Error) is 0, meaning images are identical
        printf "\r[ ${GREEN}PASS${NC}  ] Visual: %-30s\n" "${test_name}"
        rm "$actual_file" "$diff_file"
    else
        printf "\r[ ${RED}FAIL${NC}  ] Visual: %-30s\n" "${test_name}"
        failed_tests=$((failed_tests + 1))
        echo "  - Images differ. Diff image saved to: ${diff_file}"
    fi
done

echo "--------------------"
if [ "$UPDATE_MODE" -eq 1 ]; then
    echo -e "${GREEN}Visual .expected files updated.${NC}"
    exit 0
fi

if [ ${failed_tests} -gt 0 ]; then
    echo -e "${RED}Visual tests failed: ${failed_tests}/${test_count}${NC}"
    exit 1
else
    echo -e "${GREEN}All visual tests passed: ${test_count}/${test_count}${NC}"
    exit 0
fi
