#!/bin/sh

# Preamble
GREEN="\033[0;32m"
RED="\033[0;31m"
NC="\033[0m" # No Color

# API Spec Generation
PYTHON_PATH=${PYTHON_PATH:-python3.13}
echo "Generating api_spec.json..."
${PYTHON_PATH} generate_api_spec.py lv_def.json > api_spec.json
if [ ! -f api_spec.json ]; then
    echo "${RED}Error: api_spec.json not generated. Make sure generate_api_spec.py and lv_def.json are present.${NC}"
    exit 1
fi

# Test Execution Logic
cd "$(dirname "$0")"

failed_tests=0
test_count=0

for test_file in *.json; do
    test_count=$((test_count + 1))
    test_name=$(basename "${test_file}" .json)
    expected_file="${test_name}.expected"
    output_file="${test_name}.output"

    printf "[RUNNING] %-40s" "${test_name}"

    if [ ! -x ../lvgl_ui_generator ]; then
        printf "\r[ ${RED}ERROR ${NC}] %-40s (lvgl_ui_generator not found or not executable. Please build it first.)\n" "${test_name}"
        exit 1
    fi

    if [ ! -f "${expected_file}" ]; then
        printf "\r[ ${RED}SKIP  ${NC}] %-40s (Missing ${expected_file})\n" "${test_name}"
        failed_tests=$((failed_tests + 1))
        continue
    fi

    ../lvgl_ui_generator ../api_spec.json "${test_file}" > "${output_file}" 2>&1

    #diff "${output_file}" "${expected_file}"
    OUT=/dev/null
    #OUT=/tmp/`basename ${output_file}.diff`

    if diff -q "${output_file}" "${expected_file}" > "${OUT}" 2>&1; then
        printf "\r[  ${GREEN}OK   ${NC}] %-40s\n" "${test_name}"
    else
        printf "\r[ ${RED}FAIL  ${NC}] %-40s\n" "${test_name}"
        failed_tests=$((failed_tests + 1))
    fi

    # cp "${output_file}" /tmp
    rm "${output_file}"
done

# Summary
echo "--------------------"
echo "Test Summary:"
passed_tests=$((test_count - failed_tests))
echo "${passed_tests}/${test_count} tests passed."

if [ ${failed_tests} -gt 0 ]; then
    echo "${RED}${failed_tests} tests failed.${NC}"
    exit 1
else
    echo "${GREEN}All tests passed!${NC}"
    exit 0
fi
