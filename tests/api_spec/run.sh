#!/bin/bash

# Test runner for api_spec generator tests.
# Usage: ./run.sh            - run all tests
#        ./run.sh --update   - regenerate .expected files

set -e

GREEN="\033[0;32m"
RED="\033[0;31m"
YELLOW="\033[0;33m"
NC="\033[0m"

PYTHON=${PYTHON:-python3}
GENERATOR="../../generate_api_spec.py"
TEST_DIR=$(dirname "$0")

UPDATE_MODE=0
if [ "$1" = "--update" ]; then
    UPDATE_MODE=1
    echo -e "${YELLOW}--- UPDATE MODE: api_spec .expected files will be regenerated. ---${NC}"
fi

failed_tests=0
test_count=0

if [ ! -f "$GENERATOR" ]; then
    echo -e "${RED}Error: Generator script not found at '$GENERATOR'.${NC}"
    exit 1
fi

for test_json in "$TEST_DIR"/*.json; do
    test_count=$((test_count + 1))
    test_name=$(basename "${test_json}" .json)
    expected_file="${TEST_DIR}/${test_name}.expected"
    tmp_api="/tmp/${test_name}.api.json"
    tmp_out="/tmp/${test_name}.out"

    if [ "$UPDATE_MODE" -eq 1 ]; then
        echo "[UPDATING] api_spec: ${test_name}.expected"
        ${PYTHON} "$GENERATOR" "${test_json}" > "${tmp_api}"
        ${PYTHON} - <<PY > "${expected_file}"
import json
j=json.load(open("${tmp_api}"))
lines=[]
for k in sorted(j.get('constants',{})):
    lines.append(f"constants.{k}={j['constants'][k]}")
for ename in sorted(j.get('enums',{})):
    for mname in sorted(j['enums'][ename]):
        lines.append(f"enums.{ename}.{mname}={j['enums'][ename][mname]}")
print('\n'.join(lines))
PY
        continue
    fi

    if [ ! -f "${expected_file}" ]; then
        echo -e "[SKIP] api_spec: ${test_name} (Missing .expected file)."
        continue
    fi

    printf "[RUNNING] api_spec: %-30s" "${test_name}"

    ${PYTHON} "$GENERATOR" "${test_json}" > "${tmp_api}"
    ${PYTHON} - <<PY > "${tmp_out}"
import json
j=json.load(open("${tmp_api}"))
lines=[]
for k in sorted(j.get('constants',{})):
    lines.append(f"constants.{k}={j['constants'][k]}")
for ename in sorted(j.get('enums',{})):
    for mname in sorted(j['enums'][ename]):
        lines.append(f"enums.{ename}.{mname}={j['enums'][ename][mname]}")
print('\n'.join(lines))
PY

    if diff -q -w -B "${expected_file}" "${tmp_out}" > /dev/null 2>&1; then
        printf "\r[ ${GREEN}PASS${NC}  ] api_spec: %-30s\n" "${test_name}"
        rm -f "${tmp_api}" "${tmp_out}"
    else
        printf "\r[ ${RED}FAIL${NC}  ] api_spec: %-30s\n" "${test_name}"
        failed_tests=$((failed_tests + 1))
        echo "  - Diff:";
        diff -u "${expected_file}" "${tmp_out}" | sed 's/^/    /'
    fi
done

echo "--------------------"
if [ "$UPDATE_MODE" -eq 1 ]; then
    echo -e "${GREEN}api_spec .expected files updated.${NC}"
    exit 0
fi

if [ ${failed_tests} -gt 0 ]; then
    echo -e "${RED}api_spec tests failed: ${failed_tests}/${test_count}${NC}"
    exit 1
else
    echo -e "${GREEN}All api_spec tests passed: ${test_count}/${test_count}${NC}"
    exit 0
fi
