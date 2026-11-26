#!/bin/sh
set -eu

# Runs every libFuzzer harness for SECONDS each, starting from generated seeds plus the corpus a
# previous run left in build/fuzz/corpus/<name>. A crash fails the run; libFuzzer writes the
# reproducer to build/fuzz/artifacts/<name>-crash-*, and `build/fuzz/fuzz_<name> FILE` replays it.
# Usage: tools/run_fuzz.sh SECONDS NAME...

seconds=$1
shift
dir=build/fuzz
mkdir -p "$dir/artifacts"
python3 tools/fuzz_seeds.py "$dir/corpus"
fail=0
for name in "$@"; do
	echo "fuzz_$name: ${seconds}s"
	if ! "$dir/fuzz_$name" -max_total_time="$seconds" -print_final_stats=1 \
		-artifact_prefix="$dir/artifacts/$name-" "$dir/corpus/$name" \
		>"$dir/$name.log" 2>&1; then
		tail -40 "$dir/$name.log" >&2
		echo "fuzz_$name: FAILED (see $dir/$name.log and $dir/artifacts)" >&2
		fail=1
		continue
	fi
	grep -E '^stat::number_of_executed_units|^stat::peak_rss_mb' "$dir/$name.log" | tr '\n' ' '
	echo
done
exit "$fail"
