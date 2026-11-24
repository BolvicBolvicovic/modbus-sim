#!/bin/sh
# vlib's vtest always exits 0 when running whole suites, even if tests fail.
# Running tests one by one (-s suite -t name) gives a real exit code per test.
# Suites are independent, so workers run whole suites in parallel; tests inside
# a suite stay sequential. Results are collected by test index for stable output.
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

bin=${1:-./mogbus-tests}
timeout_cmd=${TEST_TIMEOUT_CMD:-timeout}
test_timeout=${TEST_TIMEOUT:-120s}
jobs=${RUN_TESTS_JOBS:-3}
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
: >"$tmp/gids"
while IFS= read -r line; do
	case "$line" in
	'For '*)
		sidx=$((sidx + 1))
		suite=${line#For }
		suite=${suite% suite:}
		printf '%s\n' "$suite" >>"$tmp/suites"
		printf '0\n' >"$tmp/count.$sidx"
		# Both suites bind a fixed port 1502, so one worker must own both.
		case "$suite" in
		tests/rtu_cli.c|tests/rtu_tcp_server.c) gid=fixed1502 ;;
		*) gid=$sidx ;;
		esac
		if [ ! -f "$tmp/group.$gid.order" ]; then
			printf '%s\n' "$gid" >>"$tmp/gids"
			printf '%s\n' "$sidx" >"$tmp/group.$gid.order"
			: >"$tmp/group.$gid.idx"
		fi
		printf '%s\n' "$sidx" >>"$tmp/group.$gid.idx"
		;;
	'setup-suite='*|'pre-test='*|'') : ;;
	*)
		if [ -z "$suite" ]; then
			printf 'run_tests.sh: test line before any suite header: %s\n' "$line" >&2
			exit 1
		fi
		total=$((total + 1))
		printf '%s\t%s\n' "$total" "$line" >>"$tmp/tests.$sidx"
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

# Groups are claimed largest-first by whichever worker becomes free, so slow
# integration suites do not cluster on one worker.
: >"$tmp/groups"
while IFS= read -r gid; do
	weight=0
	while IFS= read -r idx; do
		IFS= read -r count <"$tmp/count.$idx"
		weight=$((weight + count))
	done <"$tmp/group.$gid.idx"
	IFS= read -r order <"$tmp/group.$gid.order"
	indices=$(tr '\n' ' ' <"$tmp/group.$gid.idx")
	printf '%s %s %s\n' "$weight" "$order" "$indices" >>"$tmp/groups"
done <"$tmp/gids"
sort -k1,1nr -k2,2n "$tmp/groups" >"$tmp/queue"

if [ "$jobs" -gt "$suites" ]; then
	jobs=$suites
fi

worker_abort() {
	trap - HUP INT TERM
	if [ -n "$child" ]; then
		kill "$child" 2>/dev/null || true
	fi
	if [ "$have_lock" -eq 1 ]; then
		rmdir "$tmp/claim" 2>/dev/null || true
	fi
	exit 1
}

worker() {
	w=$1
	child=''
	have_lock=0
	trap 'worker_abort' HUP INT TERM

	while :; do
		line=''
		while :; do
			if mkdir "$tmp/claim" 2>/dev/null; then
				have_lock=1
				IFS= read -r line <"$tmp/queue" || line=''
				if [ -n "$line" ]; then
					sed '1d' "$tmp/queue" >"$tmp/queue.$w" && mv "$tmp/queue.$w" "$tmp/queue"
				fi
				rmdir "$tmp/claim"
				have_lock=0
				break
			fi
			sleep 0.01
		done
		[ -n "$line" ] || break

		set -- $line
		shift 2
		for sidx in "$@"; do
			suite=$(sed -n "${sidx}p" "$tmp/suites")
			while IFS="$tab" read -r seq test; do
				"$timeout_cmd" $timeout_kill_flag "$test_timeout" "$bin" -s "$suite" -t "$test" >"$tmp/out.$w" 2>&1 &
				child=$!
				wait "$child"
				rc=$?
				child=''
				if [ "$rc" -ne 0 ]; then
					{
						if [ "$rc" -eq 124 ]; then
							printf 'FAIL %s :: %s (timeout after %s)\n' "$suite" "$test" "$test_timeout"
						elif [ "$rc" -eq 137 ] && [ -n "$timeout_kill_flag" ]; then
							printf 'FAIL %s :: %s (killed after timeout %s)\n' "$suite" "$test" "$test_timeout"
						else
							printf 'FAIL %s :: %s (exit %s)\n' "$suite" "$test" "$rc"
						fi
						cat "$tmp/out.$w"
					} >"$tmp/result.$seq"
				fi
			done <"$tmp/tests.$sidx"
		done
	done
	printf 'ok\n' >"$tmp/worker.$w.done"
}

: >"$tmp/pids"
w=1
while [ "$w" -le "$jobs" ]; do
	worker "$w" &
	printf '%s\n' "$!" >>"$tmp/pids"
	worker_pids="$worker_pids $!"
	w=$((w + 1))
done

worker_failed=0
while IFS= read -r pid; do
	if ! wait "$pid"; then
		worker_failed=1
	fi
done <"$tmp/pids"
w=1
while [ "$w" -le "$jobs" ]; do
	if [ ! -f "$tmp/worker.$w.done" ]; then
		printf 'run_tests.sh: worker %s did not finish\n' "$w" >&2
		worker_failed=1
	fi
	w=$((w + 1))
done
if [ "$worker_failed" -ne 0 ]; then
	exit 1
fi

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

printf 'Suites: %u, tests: %u, failed: %u\n' "$suites" "$total" "$failed"
[ "$failed" -eq 0 ]
