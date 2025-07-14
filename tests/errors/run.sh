#!/bin/bash

# Test runner for error and warning reporting.
# Compares the stderr of the generator against a .expected file.
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
    echo -e "${YELLOW}--- UPDATE MODE ENABLED: Error .expected files will be regenerated. ---${NC}"
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
    expected_file="${TEST_DIR}/${test_name}.stderr.expected"
    output_stderr="/tmp/${test_name}.stderr.actual"
    sanitized_stderr="/tmp/${test_name}.stderr.sanitized"

    # Run the generator and capture stderr. We expect a non-zero exit code for most, so we use `|| true`
    "$GENERATOR_EXE" "$API_SPEC_PATH" "$test_yaml" > /dev/null 2> "$output_stderr" || true
    
    # Sanitize the output to remove file paths which can change between environments
    sed 's|Path:.*|Path: [REDACTED]|' "$output_stderr" > "$sanitized_stderr"

    if [ "$UPDATE_MODE" -eq 1 ]; then
        echo "[UPDATING] Error Test: ${test_name}.stderr.expected"
        cp "$sanitized_stderr" "$expected_file"
        rm "$output_stderr" "$sanitized_stderr"
        continue
    fi

    if [ ! -f "$expected_file" ]; then
        echo -e "[${YELLOW}SKIP${NC}] Error Test: ${test_name} (No .stderr.expected file. Run with --update to create.)"
        continue
    fi

    printf "[RUNNING] Error Test: %-26s" "${test_name}"

    if diff -q -w -B "$expected_file" "$sanitized_stderr" > /dev/null 2>&1; then
        printf "\r[ ${GREEN}PASS${NC}  ] Error Test: %-26s\n" "${test_name}"
        rm "$output_stderr" "$sanitized_stderr"
    else
        printf "\r[ ${RED}FAIL${NC}  ] Error Test: %-26s\n" "${test_name}"
        failed_tests=$((failed_tests + 1))
        echo "  - Diff:"
        diff -u "$expected_file" "$sanitized_stderr" | sed 's/^/    /'
    fi
done

echo "--------------------"
if [ "$UPDATE_MODE" -eq 1 ]; then
    echo -e "${GREEN}Error .expected files updated.${NC}"
    exit 0
fi

if [ ${failed_tests} -gt 0 ]; then
    echo -e "${RED}Error tests failed: ${failed_tests}/${test_count}${NC}"
    exit 1
else
    echo -e "${GREEN}All error tests passed: ${test_count}/${test_count}${NC}"
    exit 0
fi
