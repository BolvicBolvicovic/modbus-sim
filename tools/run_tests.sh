#!/bin/sh
# vlib's vtest always exits 0 when running whole suites, even if tests fail.
# Running tests one by one (-s suite -t name) gives a real exit code per test.
set -u

usage() {
	printf 'usage: %s [--self-test] [test-binary]\n' "$0" >&2
	exit 2
}

self_test() {
	tmp=$(mktemp -d "${TMPDIR:-/tmp}/run_tests.XXXXXX")
	child_pid_file=$tmp/child.pid
	fake=$tmp/fake-test
	out=$tmp/out
	err=$tmp/err
	trap 'rm -rf "$tmp"' EXIT HUP INT TERM

	cat >"$fake" <<'FAKE'
#!/bin/sh
set -eu
case "${1:-}" in
--list)
	printf 'For hang suite:\n'
	printf 'sleep_forever\n'
	;;
-s)
	if [ "${2:-}" = "hang" ] && [ "${3:-}" = "-t" ] && [ "${4:-}" = "sleep_forever" ]; then
		sleep 300 &
		child=$!
		if [ -n "${RUN_TESTS_CHILD_PID_FILE:-}" ]; then
			printf '%s\n' "$child" >"$RUN_TESTS_CHILD_PID_FILE"
		fi
		sleep 300
	fi
	printf 'unknown test\n' >&2
	exit 1
	;;
*)
	printf 'unknown args\n' >&2
	exit 1
	;;
esac
FAKE
	chmod +x "$fake"

	RUN_TESTS_CHILD_PID_FILE="$child_pid_file" \
		TEST_TIMEOUT=1s \
		"$0" "$fake" >"$out" 2>"$err"
	rc=$?
	if [ "$rc" -eq 0 ]; then
		printf 'run_tests.sh self-test: expected timeout failure\n' >&2
		cat "$out" >&2
		cat "$err" >&2
		exit 1
	fi

	if ! grep -q 'FAIL hang :: sleep_forever (timeout after 1s)' "$out"; then
		printf 'run_tests.sh self-test: missing timeout failure report\n' >&2
		cat "$out" >&2
		cat "$err" >&2
		exit 1
	fi

	child=''
	if [ -f "$child_pid_file" ]; then
		child=$(cat "$child_pid_file")
	fi
	if [ -z "$child" ]; then
		printf 'run_tests.sh self-test: missing child pid\n' >&2
		cat "$out" >&2
		cat "$err" >&2
		exit 1
	fi

	for _ in 1 2 3 4 5 6 7 8 9 10; do
		if kill -0 "$child" 2>/dev/null; then
			sleep 1
		else
			child=''
			break
		fi
	done
	if [ -n "$child" ] && kill -0 "$child" 2>/dev/null; then
		printf 'run_tests.sh self-test: orphaned child still running: %s\n' "$child" >&2
		kill -9 "$child" 2>/dev/null || true
		cat "$out" >&2
		cat "$err" >&2
		exit 1
	fi

	printf 'run_tests.sh self-test: ok\n'
}

case "${1:-}" in
--self-test)
	self_test
	exit 0
	;;
-h|--help)
	usage
	;;
esac

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

timeout_kill_flag=''
if "$timeout_cmd" -k 1 1s true >/dev/null 2>&1; then
	timeout_kill_flag='-k 5s'
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
			# shellcheck disable=SC2086
			"$timeout_cmd" $timeout_kill_flag "$test_timeout" "$bin" -s "$suite" -t "$line" >"$out" 2>&1
			rc=$?
			if [ "$rc" -ne 0 ]; then
				failed=$((failed + 1))
				if [ "$rc" -eq 124 ]; then
					printf 'FAIL %s :: %s (timeout after %s)\n' "$suite" "$line" "$test_timeout"
				elif [ "$rc" -eq 137 ] && [ -n "$timeout_kill_flag" ]; then
					printf 'FAIL %s :: %s (killed after timeout %s)\n' "$suite" "$line" "$test_timeout"
				else
					printf 'FAIL %s :: %s (exit %s)\n' "$suite" "$line" "$rc"
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
