#!/bin/sh
cd "$(dirname "$0")" || exit 1
if [ ! -f ../managed_components/espressif__cjson/cJSON/cJSON.c ]; then
    echo "managed_components/espressif__cjson is missing. From the repository root, with ESP-IDF v6.1 activated, run:" >&2
    echo "    idf.py reconfigure" >&2
    exit 1
fi
pass=0
fail=0
failed=""
for test in test_*.py; do
    if python3 "$test" >/dev/null 2>&1; then
        echo "PASS $test"
        pass=$((pass + 1))
    else
        echo "FAIL $test"
        fail=$((fail + 1))
        failed="$failed $test"
    fi
done
for check in ../components/esp32-ws154/check-host.sh; do
    module=$(basename "$(dirname "$check")")
    output=$(sh "$check" 2>&1) && status=0 || status=$?
    module_pass=$(echo "$output" | grep -c '^PASS ' || true)
    module_fail=$(echo "$output" | grep -c '^FAIL ' || true)
    echo "$output" | grep -E '^(PASS|FAIL) ' | sed "s#^\([A-Z]*\) #\1 $module/#"
    if [ "$status" -ne 0 ] && [ "$module_fail" -eq 0 ]; then
        echo "FAIL $module/check-host.sh"
        echo "$output" | tail -20
        module_fail=1
    fi
    pass=$((pass + module_pass))
    fail=$((fail + module_fail))
    [ "$module_fail" -eq 0 ] || failed="$failed $module"
done
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ] || { echo "Failed:$failed"; exit 1; }
