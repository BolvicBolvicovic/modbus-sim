#!/bin/sh
set -eu

# Runs every libFuzzer harness for SECONDS each, at most FUZZ_JOBS (default nproc) at once, starting
# from generated seeds plus the corpus a previous run left in build/fuzz/corpus/<name>. A crash fails
# the run; libFuzzer writes the reproducer to build/fuzz/artifacts/<name>-crash-*, and
# `build/fuzz/fuzz_<name> FILE` replays it. Results print in argument order.
# Usage: tools/run_fuzz.sh SECONDS NAME...

seconds=$1
shift
dir=build/fuzz
jobs=${FUZZ_JOBS:-$(nproc)}
mkdir -p "$dir/artifacts"
python3 tools/fuzz_seeds.py "$dir/corpus"

run_one() {
	if "$dir/fuzz_$1" -max_total_time="$seconds" -print_final_stats=1 \
		-artifact_prefix="$dir/artifacts/$1-" "$dir/corpus/$1" \
		>"$dir/$1.log" 2>&1; then
		echo 0 >"$dir/$1.status"
	else
		echo 1 >"$dir/$1.status"
	fi
}

echo "fuzzing $# harnesses, ${seconds}s each, up to $jobs at once"
running=0
for name in "$@"; do
	rm -f "$dir/$name.status"
	if [ "$running" -ge "$jobs" ]; then
		wait
		running=0
	fi
	run_one "$name" &
	running=$((running + 1))
done
wait

fail=0
for name in "$@"; do
	echo "fuzz_$name: ${seconds}s"
	if [ "$(cat "$dir/$name.status" 2>/dev/null || echo 1)" != 0 ]; then
		tail -40 "$dir/$name.log" >&2
		echo "fuzz_$name: FAILED (see $dir/$name.log and $dir/artifacts)" >&2
		fail=1
		continue
	fi
	grep -E '^stat::number_of_executed_units|^stat::peak_rss_mb' "$dir/$name.log" | tr '\n' ' '
	echo
done
exit "$fail"
