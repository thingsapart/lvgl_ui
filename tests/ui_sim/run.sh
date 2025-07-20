#!/bin/bash

# Test runner for UI-Sim logic tests.
# Runs the UI-Sim for a set number of ticks and compares the execution
# trace against a .expected file.
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
NORMALIZER="${TEST_DIR}/normalize_trace.py"

UPDATE_MODE=0
if [ "$1" = "--update" ]; then
    UPDATE_MODE=1
    echo -e "${YELLOW}--- UPDATE MODE ENABLED: UI-Sim trace .expected files will be regenerated. ---${NC}"
fi

failed_tests=0
test_count=0

if [ ! -x "$GENERATOR_EXE" ]; then
    echo -e "${RED}Error: Generator executable not found at '$GENERATOR_EXE'. Please build it first.${NC}"
    exit 1
fi

if [ ! -x "$NORMALIZER" ]; then
    echo -e "${RED}Error: Normalizer script not found or not executable at '$NORMALIZER'.${NC}"
    exit 1
fi


for test_yaml in "$TEST_DIR"/*.yaml; do
    test_count=$((test_count + 1))
    test_name=$(basename "${test_yaml}" .yaml)
    expected_file="${TEST_DIR}/${test_name}.trace.expected"
    raw_output_file="/tmp/${test_name}.trace.raw"
    normalized_output_file="/tmp/${test_name}.trace.actual"

    # Extract number of ticks from the first line of the YAML file, e.g., "# TICKS: 5"
    ticks=$(grep '^# TICKS:' "$test_yaml" | awk '{print $3}')
    if [ -z "$ticks" ]; then
        ticks=1 # Default to 1 tick if not specified
    fi

    if [ "$UPDATE_MODE" -eq 1 ]; then
        echo "[UPDATING] UI-Sim Trace: ${test_name}.trace.expected (Ticks: $ticks)"
        # When updating, we also normalize the output to ensure consistency
        "$GENERATOR_EXE" --run-sim-test "$ticks" --api-spec "$API_SPEC_PATH" --ui-spec "$test_yaml" > "$raw_output_file"
        "$NORMALIZER" "$raw_output_file" > "$expected_file"
        rm "$raw_output_file"
        continue
    fi

    if [ ! -f "$expected_file" ]; then
        echo -e "[${YELLOW}SKIP${NC}] UI-Sim: ${test_name} (No .trace.expected file. Run with --update to create.)"
        continue
    fi

    printf "[RUNNING] UI-Sim: %-30s" "${test_name}"

    # Generate raw output, then normalize it before comparing
    "$GENERATOR_EXE" --run-sim-test "$ticks" --api-spec "$API_SPEC_PATH" --ui-spec "$test_yaml" > "$raw_output_file"
    "$NORMALIZER" "$raw_output_file" > "$normalized_output_file"


    if diff -q -w -B "$expected_file" "$normalized_output_file" > /dev/null 2>&1; then
        printf "\r[ ${GREEN}PASS${NC}  ] UI-Sim: %-30s\n" "${test_name}"
        rm "$raw_output_file" "$normalized_output_file"
    else
        printf "\r[ ${RED}FAIL${NC}  ] UI-Sim: %-30s\n" "${test_name}"
        failed_tests=$((failed_tests + 1))
        echo "  - Diff:"
        # Diff against the normalized output
        diff -u "$expected_file" "$normalized_output_file" | sed 's/^/    /'
    fi
done

echo "--------------------"
if [ "$UPDATE_MODE" -eq 1 ]; then
    echo -e "${GREEN}UI-Sim trace .expected files updated.${NC}"
    exit 0
fi

if [ ${failed_tests} -gt 0 ]; then
    echo -e "${RED}UI-Sim tests failed: ${failed_tests}/${test_count}${NC}"
    exit 1
else
    echo -e "${GREEN}All UI-Sim tests passed: ${test_count}/${test_count}${NC}"
    exit 0
fi
