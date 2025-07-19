#!/bin/bash

# Test runner for the YAML Parser.
# Compares the JSON output of the parser against a .json.expected file.
#
# Usage:
#   ./run.sh          - Run all tests and compare against .expected files.
#   ./run.sh --update - Regenerate all .json.expected files with the current output.

# set -e

GREEN="\033[0;32m"
RED="\033[0;31m"
YELLOW="\033[0;33m"
NC="\033[0m"

GENERATOR_EXE="../../lvgl_ui_generator"
TEST_DIR=$(dirname "$0")

UPDATE_MODE=0
if [ "$1" = "--update" ]; then
    UPDATE_MODE=1
    echo -e "${YELLOW}--- UPDATE MODE ENABLED: .json.expected files will be regenerated. ---${NC}"
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
    expected_file="${TEST_DIR}/${test_name}.json.expected"
    output_file="/tmp/${test_name}.json.actual"

    if [ "$UPDATE_MODE" -eq 1 ]; then
        echo "[UPDATING] Parser test: ${test_name}.json.expected"
        "$GENERATOR_EXE" --parse-yaml-to-json "$test_yaml" > "$expected_file"
        continue
    fi

    if [ ! -f "$expected_file" ]; then
        echo -e "[${YELLOW}SKIP${NC}] Parser test: ${test_name} (No .json.expected file. Run with --update to create.)"
        continue
    fi

    printf "[RUNNING] Parser test: %-35s" "${test_name}"

    if ! "$GENERATOR_EXE" --parse-yaml-to-json "$test_yaml" > "$output_file"; then
        printf "\r[ ${RED}FAIL${NC}  ] Parser test: %-35s (Generator Crashed)\n" "${test_name}"
        failed_tests=$((failed_tests + 1))
        # Print generator's stderr
        "$GENERATOR_EXE" --parse-yaml-to-json "$test_yaml" > /dev/null
        continue
    fi


    if diff -q -w -B "$expected_file" "$output_file" > /dev/null 2>&1; then
        printf "\r[ ${GREEN}PASS${NC}  ] Parser test: %-35s\n" "${test_name}"
        rm "$output_file"
    else
        printf "\r[ ${RED}FAIL${NC}  ] Parser test: %-35s\n" "${test_name}"
        failed_tests=$((failed_tests + 1))
        echo "  - Diff:"
        diff -u "$expected_file" "$output_file" | sed 's/^/    /'
    fi
done

echo "--------------------"
if [ "$UPDATE_MODE" -eq 1 ]; then
    echo -e "${GREEN}YAML Parser .json.expected files updated.${NC}"
    exit 0
fi

if [ ${failed_tests} -gt 0 ]; then
    echo -e "${RED}YAML Parser tests failed: ${failed_tests}/${test_count}${NC}"
    exit 1
else
    echo -e "${GREEN}All YAML Parser tests passed: ${test_count}/${test_count}${NC}"
    exit 0
fi
