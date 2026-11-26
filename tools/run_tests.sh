#!/bin/sh
# vlib's vtest always exits 0 when running whole suites, even if tests fail.
# Running tests one by one (-s suite -t name) gives a real exit code per test.
# Every test runs in its own process and integration tests pick free ports, so
# xargs -P workers take single tests from one queue. The queue starts with the
# slowest tests of the previous run (durations cached in BIN.times, or
# RUN_TESTS_TIMES), so long tests do not end up last; tests without a recorded
# time go first. Results are collected by test index for stable output.
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

# One queue line "seq<TAB>suite-index<TAB>test", run by an xargs worker.
run_one() {
	tab=$(printf '\t')
	IFS="$tab" read -r seq sidx test <<EOF_LINE
$1
EOF_LINE
	tmp=$RUN_TESTS_TMP
	suite=$(sed -n "${sidx}p" "$tmp/suites")
	out=$tmp/out.$seq
	start=$(date +%s%N)
	# shellcheck disable=SC2086
	"$RUN_TESTS_TIMEOUT_CMD" $RUN_TESTS_KILL_FLAG "$RUN_TESTS_TIMEOUT" \
		"$RUN_TESTS_BIN" -s "$suite" -t "$test" >"$out" 2>&1
	rc=$?
	printf '%s\t%s\t%s\n' "$(( ($(date +%s%N) - start) / 1000000 ))" "$suite" "$test" >"$tmp/time.$seq"
	if [ "$rc" -ne 0 ]; then
		{
			if [ "$rc" -eq 124 ]; then
				printf 'FAIL %s :: %s (timeout after %s)\n' "$suite" "$test" "$RUN_TESTS_TIMEOUT"
			elif [ "$rc" -eq 137 ] && [ -n "$RUN_TESTS_KILL_FLAG" ]; then
				printf 'FAIL %s :: %s (killed after timeout %s)\n' "$suite" "$test" "$RUN_TESTS_TIMEOUT"
			else
				printf 'FAIL %s :: %s (exit %s)\n' "$suite" "$test" "$rc"
			fi
			cat "$out"
		} >"$tmp/result.$seq"
	fi
	rm -f "$out"
	return 0
}

case "${1:-}" in
--run-one)
	run_one "$2"
	exit 0
	;;
--self-test)
	self_test
	exit 0
	;;
-h|--help)
	usage
	;;
esac

bin=${1:-./mogbus-tests}
timeout_cmd=${TEST_TIMEOUT_CMD:-timeout}
test_timeout=${TEST_TIMEOUT:-120s}
jobs=${RUN_TESTS_JOBS:-$(nproc 2>/dev/null || echo 4)}
case "$jobs" in
''|*[!0-9]*) printf 'run_tests.sh: invalid RUN_TESTS_JOBS: %s\n' "$jobs" >&2; exit 1 ;;
esac
if [ "$jobs" -lt 1 ]; then
	printf 'run_tests.sh: RUN_TESTS_JOBS must be at least 1\n' >&2
	exit 1
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/run_tests.XXXXXX")
tab=$(printf '\t')
worker_pids=''

cleanup() {
	rm -rf "$tmp"
}
terminate() {
	trap - EXIT HUP INT TERM
	if [ -n "$worker_pids" ]; then
		# shellcheck disable=SC2086
		kill $worker_pids 2>/dev/null || true
		# shellcheck disable=SC2086
		wait $worker_pids 2>/dev/null || true
	fi
	cleanup
	exit 1
}
trap cleanup EXIT
trap terminate HUP INT TERM

if ! command -v "$timeout_cmd" >/dev/null 2>&1; then
	printf 'run_tests.sh: timeout command not found: %s\n' "$timeout_cmd" >&2
	exit 1
fi

timeout_kill_flag=''
if "$timeout_cmd" -k 1 1s true >/dev/null 2>&1; then
	timeout_kill_flag='-k 5s'
fi

list=$tmp/list
if ! "$bin" --list >"$list"; then
	printf 'run_tests.sh: failed to list tests from %s\n' "$bin" >&2
	exit 1
fi

suite=''
sidx=0
total=0
: >"$tmp/suites"
: >"$tmp/all"
while IFS= read -r line; do
	case "$line" in
	'For '*)
		sidx=$((sidx + 1))
		suite=${line#For }
		suite=${suite% suite:}
		printf '%s\n' "$suite" >>"$tmp/suites"
		printf '0\n' >"$tmp/count.$sidx"
		;;
	'setup-suite='*|'pre-test='*|'') : ;;
	*)
		if [ -z "$suite" ]; then
			printf 'run_tests.sh: test line before any suite header: %s\n' "$line" >&2
			exit 1
		fi
		total=$((total + 1))
		printf '%s\t%s\t%s\n' "$total" "$sidx" "$line" >>"$tmp/all"
		IFS= read -r count <"$tmp/count.$sidx"
		printf '%s\n' "$((count + 1))" >"$tmp/count.$sidx"
		;;
	esac
done <"$list"

suites=$sidx
if [ "$total" -eq 0 ]; then
	printf 'run_tests.sh: no tests discovered in --list output from %s\n' "$bin" >&2
	exit 1
fi

# Queue lines are "ms<TAB>seq<TAB>suite-index<TAB>test", slowest first.
cache=${RUN_TESTS_TIMES:-$bin.times}
times=$cache
[ -r "$times" ] || times=/dev/null
awk -F "$tab" -v OFS="$tab" '
	NR == FNR { ms[$2 FS $3] = $1; next }
	FILENAME ~ /suites$/ { name[FNR] = $0; next }
	{ key = name[$2] FS $3; print (key in ms ? ms[key] : 999999999), $1, $2, $3 }
' "$times" "$tmp/suites" "$tmp/all" | sort -t "$tab" -k1,1nr -k2,2n >"$tmp/queue"

if [ "$jobs" -gt "$total" ]; then
	jobs=$total
fi

# xargs -P hands queue lines to free workers; each line runs one test via
# --run-one, which writes result.SEQ only on failure.
cut -f2- "$tmp/queue" |
	RUN_TESTS_TMP=$tmp RUN_TESTS_BIN=$bin RUN_TESTS_TIMEOUT_CMD=$timeout_cmd \
	RUN_TESTS_TIMEOUT=$test_timeout RUN_TESTS_KILL_FLAG=$timeout_kill_flag \
	xargs -d '\n' -n 1 -P "$jobs" "$0" --run-one &
worker_pids=$!
if ! wait "$worker_pids"; then
	worker_pids=''
	printf 'run_tests.sh: a test worker failed to run\n' >&2
	exit 1
fi
worker_pids=''

failed=0
seq=1
while [ "$seq" -le "$total" ]; do
	result="$tmp/result.$seq"
	if [ -s "$result" ]; then
		failed=$((failed + 1))
		cat "$result"
	fi
	seq=$((seq + 1))
done

# Best effort: an unwritable cache only loses the ordering hint.
cat "$tmp"/time.* >"$tmp/times" 2>/dev/null && cp "$tmp/times" "$cache" 2>/dev/null

printf 'Suites: %u, tests: %u, failed: %u\n' "$suites" "$total" "$failed"
[ "$failed" -eq 0 ]
