#!/bin/sh
# vlib's vtest always exits 0 when running whole suites, even if tests fail.
# Running tests one by one (-s suite -t name) gives a real exit code per test.
set -u

bin=${1:-./modbus-sim-tests}
timeout_cmd=${TEST_TIMEOUT_CMD:-timeout}
test_timeout=${TEST_TIMEOUT:-120s}
list=$(mktemp)
out=$(mktemp)
trap 'rm -f "$list" "$out"' EXIT HUP INT TERM

if ! command -v "$timeout_cmd" >/dev/null 2>&1; then
	printf 'run_tests.sh: timeout command not found: %s\n' "$timeout_cmd" >&2
	exit 1
fi

if ! "$bin" --list >"$list"; then
	printf 'run_tests.sh: failed to list tests from %s\n' "$bin" >&2
	exit 1
fi

suite=''
suites=0
total=0
failed=0

while IFS= read -r line; do
	case "$line" in
		'For '*)
			suite=${line#For }
			suite=${suite% suite:}
			suites=$((suites + 1))
			;;
		'setup-suite='*|'pre-test='*|'') : ;;
		*)
			if [ -z "$suite" ]; then
				printf 'run_tests.sh: test line before any suite header: %s\n' "$line" >&2
				exit 1
			fi
			total=$((total + 1))
			if ! "$timeout_cmd" --foreground -k 5s "$test_timeout" "$bin" -s "$suite" -t "$line" >"$out" 2>&1; then
				rc=$?
				failed=$((failed + 1))
				if [ "$rc" -eq 124 ]; then
					printf 'FAIL %s :: %s (timeout after %s)\n' "$suite" "$line" "$test_timeout"
				else
					printf 'FAIL %s :: %s\n' "$suite" "$line"
				fi
				cat "$out"
			fi
			;;
	esac
done <"$list"

if [ "$total" -eq 0 ]; then
	printf 'run_tests.sh: no tests discovered in --list output from %s\n' "$bin" >&2
	exit 1
fi

printf 'Suites: %u, tests: %u, failed: %u\n' "$suites" "$total" "$failed"
[ "$failed" -eq 0 ]
