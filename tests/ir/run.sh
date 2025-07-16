#!/bin/bash

# Test runner for Intermediate Representation (IR) tests.
# Compares the output of the 'ir_debug_print' backend against a .expected file.
#
# Usage:
#   ./run.sh          - Run all tests and compare against .expected files.
#   ./run.sh --update - Regenerate all .expected files with the current output.

set -e

GREEN="\033[0;32m"
RED="\033[0;31m"
YELLOW="\033[0;33m"
NC="\033[0m"

GENERATOR_EXE="../../lvgl_ui_generator"
API_SPEC_PATH="../../api_spec.json"
TEST_DIR=$(dirname "$0")

UPDATE_MODE=0
if [ "$1" = "--update" ]; then
    UPDATE_MODE=1
    echo -e "${YELLOW}--- UPDATE MODE ENABLED: IR .expected files will be regenerated. ---${NC}"
fi

failed_tests=0
test_count=0

if [ ! -x "$GENERATOR_EXE" ]; then
    echo -e "${RED}Error: Generator executable not found at '$GENERATOR_EXE'. Please build it first.${NC}"
    exit 1
fi

# The sed command to normalize non-deterministic pointer addresses
SANITIZE_CMD="s/ptr=0x[0-9a-f]*/ptr=0xcafe/g"

for test_yaml in "$TEST_DIR"/*.yaml; do
    test_count=$((test_count + 1))
    test_name=$(basename "${test_yaml}" .yaml)
    expected_file="${TEST_DIR}/${test_name}.ir.expected"
    output_file="/tmp/${test_name}.ir.actual"

    if [ "$UPDATE_MODE" -eq 1 ]; then
        echo "[UPDATING] IR: ${test_name}.ir.expected"
        # In update mode, generate the output and immediately sanitize it before saving.
        "$GENERATOR_EXE" "$API_SPEC_PATH" "$test_yaml" --codegen ir_debug_print | sed "$SANITIZE_CMD" > "$expected_file"
        continue
    fi

    if [ ! -f "$expected_file" ]; then
        echo -e "[${YELLOW}SKIP${NC}] IR: ${test_name} (No .ir.expected file. Run with --update to create.)"
        continue
    fi

    printf "[RUNNING] IR: %-30s" "${test_name}"

    # Generate the raw output from the tool
    "$GENERATOR_EXE" "$API_SPEC_PATH" "$test_yaml" --codegen ir_debug_print > "$output_file"

    # Compare the sanitized versions of the expected and actual files.
    # Process substitution <(...) is used to feed the output of the sed commands directly to diff.
    # The diff output is captured, and `|| true` prevents the script from exiting on a mismatch.
    sanitized_diff=$(diff -u -w -B \
        <(sed "$SANITIZE_CMD" "$expected_file") \
        <(sed "$SANITIZE_CMD" "$output_file") \
        || true)

    if [ -z "$sanitized_diff" ]; then
        printf "\r[ ${GREEN}PASS${NC}  ] IR: %-30s\n" "${test_name}"
        rm "$output_file"
    else
        printf "\r[ ${RED}FAIL${NC}  ] IR: %-30s\n" "${test_name}"
        failed_tests=$((failed_tests + 1))
        echo "  - Diff (pointers sanitized):"
        echo "$sanitized_diff" | sed 's/^/    /'
    fi
done

echo "--------------------"
if [ "$UPDATE_MODE" -eq 1 ]; then
    echo -e "${GREEN}IR .expected files updated.${NC}"
    exit 0
fi

if [ ${failed_tests} -gt 0 ]; then
    echo -e "${RED}IR tests failed: ${failed_tests}/${test_count}${NC}"
    exit 1
else
    echo -e "${GREEN}All IR tests passed: ${test_count}/${test_count}${NC}"
    exit 0
fi
