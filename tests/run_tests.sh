#!/bin/bash
# set -x # Debug tracing REMOVED for now

# Preamble
GREEN="\033[0;32m"
RED="\033[0;31m"
NC="\033[0m" # No Color

# Default debug mode to off
DEBUG_MODE=0
SINGLE_TEST_NAME=""

# Parse command-line arguments
while [ "$#" -gt 0 ]; do
    case "$1" in
        -d|--debug)
            DEBUG_MODE=1
            shift # past argument
            ;;
        *)
            # If it's not a flag, assume it's the test name
            if [ -z "$SINGLE_TEST_NAME" ]; then
                SINGLE_TEST_NAME=$1
            else
                echo "Error: Only one test name can be specified."
                exit 1
            fi
            shift # past argument
            ;;
    esac
done

# Change directory if running from root
if [ -f "./api_spec.json" ] && [ -d "./tests" ]; then
    echo "Changing directory to ./tests"
    cd ./tests
fi

# API Spec Generation
# PYTHON_PATH=${PYTHON_PATH:-python3}
# echo "Generating api_spec.json..."
# ${PYTHON_PATH} ../generate_api_spec.py ../lv_def.json > ../api_spec.json
if [ ! -f ../api_spec.json ]; then # Reverted to [
    echo "${RED}Error: api_spec.json not generated. Make sure generate_api_spec.py and lv_def.json are present.${NC}"
    exit 1
fi

# Test Execution Logic
# cd "$(dirname "$0")" # Already in tests directory

failed_tests=0
test_count=0

GEN=./lvgl_ui_generator
if [ ! -x "${GEN}" ]; then # Reverted to [ and unquoted GEN
  GEN=../lvgl_ui_generator
fi

# Determine test files to run and loop
if [ -n "$SINGLE_TEST_NAME" ]; then
    if [ ! -f "${SINGLE_TEST_NAME}.json" ]; then
        echo "Error: Test file ${SINGLE_TEST_NAME}.json not found."
        exit 1
    fi
    # Process single test
    test_file="${SINGLE_TEST_NAME}.json"
    test_count=$((test_count + 1))
    test_name=$(basename "${test_file}" .json)
    expected_file="${test_name}.expected"
    output_file="/tmp/${test_name}.output"

    if [ -f "${expected_file}" ]; then
      printf "[RUNNING] %-40s\n" "${test_name}" # Restored
    fi

    if [ ! -x "${GEN}" ]; then
        printf "\r[ ${RED}ERROR ${NC}] %-40s (lvgl_ui_generator not found or not executable. Please build it first.)\n" "${test_name}" # Restored
        exit 1
    fi

    if [ ! -f "${expected_file}" ]; then
        # This case should ideally not be hit for a single specified test
        # but keeping logic consistent if expected file is missing.
        echo "Warning: Expected file ${expected_file} not found for single test ${test_name}."
        # failed_tests=$((failed_tests + 1)) # Decide if this should count as failure
    else
        "${GEN}" ../api_spec.json "${test_file}" --codegen c_code > "${output_file}" 2>&1
        OUT=/dev/null
        if diff -q "${output_file}" "${expected_file}" > "${OUT}" 2>&1; then
            printf "\r[  ${GREEN}OK   ${NC}] %-40s\n" "${test_name}"
        else
            printf "\r[ ${RED}FAIL  ${NC}] %-40s\n" "${test_name}"
            failed_tests=$((failed_tests + 1))
            if [ "${DEBUG_MODE}" -eq 1 ]; then
                echo "Diff for ${test_name}:"
                diff -u "${expected_file}" "${output_file}"
                echo "--------------------"
            fi
        fi
        if [ "${DEBUG_MODE}" -eq 0 ]; then
            rm "${output_file}"
        fi
    fi
else
    # Process all *.json files
    for test_file_glob in *.json; do
        # Check if glob found any files to prevent literal '*.json' processing
        if [ ! -f "$test_file_glob" ] && [ "$test_file_glob" = "*.json" ]; then
            echo "No .json test files found."
            break
        fi

        test_file="$test_file_glob" # Use the matched file
        test_count=$((test_count + 1))
        test_name=$(basename "${test_file}" .json)
        expected_file="${test_name}.expected"
        output_file="/tmp/${test_name}.output"

        if [ -f "${expected_file}" ]; then
          printf "[RUNNING] %-40s\n" "${test_name}" # Restored
        fi

        if [ ! -x "${GEN}" ]; then
            printf "\r[ ${RED}ERROR ${NC}] %-40s (lvgl_ui_generator not found or not executable. Please build it first.)\n" "${test_name}" # Restored
            exit 1 # Exit for all tests if generator is missing
        fi

        if [ ! -f "${expected_file}" ]; then
            # printf "\r[ ${RED}SKIP  ${NC}] %-40s (Missing ${expected_file})\n" "${test_name}"
            # failed_tests=$((failed_tests + 1))
            continue
        fi

        "${GEN}" ../api_spec.json "${test_file}" --codegen c_code > "${output_file}" 2>&1
        OUT=/dev/null
        if diff -q "${output_file}" "${expected_file}" > "${OUT}" 2>&1; then
            printf "\r[  ${GREEN}OK   ${NC}] %-40s\n" "${test_name}"
        else
            printf "\r[ ${RED}FAIL  ${NC}] %-40s\n" "${test_name}"
            failed_tests=$((failed_tests + 1))
            if [ "${DEBUG_MODE}" -eq 1 ]; then
                echo "Diff for ${test_name}:"
                diff -u "${expected_file}" "${output_file}"
                echo "--------------------"
            fi
        fi
        if [ "${DEBUG_MODE}" -eq 0 ]; then
            rm "${output_file}"
        fi
    done
fi

# Restore original printf lines after testing this structural change
# Summary is outside the modified loop structure
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
    expected_file="${test_name}.expected"
    output_file="/tmp/${test_name}.output"

    if [ -f "${expected_file}" ]; then # Reverted to [
      echo "Placeholder for running" # Simplified printf
    fi

    if [ ! -x "${GEN}" ]; then # Reverted to [
        echo "Placeholder for error" # Simplified printf
        exit 1
    fi

    if [ ! -f "${expected_file}" ]; then # Reverted to [
        # printf "\r[ ${RED}SKIP  ${NC}] %-40s (Missing ${expected_file})\n" "${test_name}"
        # failed_tests=$((failed_tests + 1))
        continue
    fi

    "${GEN}" ../api_spec.json "${test_file}" --codegen c_code > "${output_file}" 2>&1 # Quoted GEN

    #diff "${output_file}" "${expected_file}"
    OUT=/dev/null
    #OUT=/tmp/`basename ${output_file}.diff`

    if diff -q "${output_file}" "${expected_file}" > "${OUT}" 2>&1; then
        printf "\r[  ${GREEN}OK   ${NC}] %-40s\n" "${test_name}"
    else
        printf "\r[ ${RED}FAIL  ${NC}] %-40s\n" "${test_name}"
        failed_tests=$((failed_tests + 1))
        if [ "${DEBUG_MODE}" -eq 1 ]; then
            echo "Diff for ${test_name}:"
            diff -u "${expected_file}" "${output_file}"
            echo "--------------------"
        fi
    fi

    # cp "${output_file}" /tmp
    if [ "${DEBUG_MODE}" -eq 0 ]; then
        rm "${output_file}"
    fi
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

