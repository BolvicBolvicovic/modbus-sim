#!/bin/sh
# vlib's vtest always exits 0 when running whole suites, even if tests fail.
# Running tests one by one (-s suite -t name) gives a real exit code per test.
set -u

bin=${1:-./modbus-sim-tests}
list=$(mktemp)
out=$(mktemp)
trap 'rm -f "$list" "$out"' EXIT HUP INT TERM

"$bin" --list >"$list" || exit 1

suite=''
total=0
failed=0

while IFS= read -r line; do
	case "$line" in
		'For '*) suite=${line#For }; suite=${suite% suite:} ;;
		'setup-suite='*|'pre-test='*|'') : ;;
		*)
			total=$((total + 1))
			if ! "$bin" -s "$suite" -t "$line" >"$out" 2>&1; then
				failed=$((failed + 1))
				printf 'FAIL %s :: %s\n' "$suite" "$line"
				cat "$out"
			fi
			;;
	esac
done <"$list"

printf 'Tests: %u, failed: %u\n' "$total" "$failed"
[ "$failed" -eq 0 ]
