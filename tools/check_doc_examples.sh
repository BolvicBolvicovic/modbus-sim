#!/bin/sh
# Extract fenced ```scheme blocks from docs/README and run --check on blocks
# that look like complete configs (contain a top-level (device ...) form).
set -eu

bin=${1:-./modbus-sim}
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/modbus-sim-doccheck.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

status="$tmpdir/status"
echo 0 > "$status"

extract() {
	awk -v file="$1" -v dir="$tmpdir" '
	function safename(s,    t) {
		t = s
		gsub(/[^A-Za-z0-9._-]/, "_", t)
		return t
	}
	/^```scheme[[:space:]]*$/ { inb=1; n++; block=""; next }
	/^```[[:space:]]*$/ && inb {
		inb=0
		if (block ~ /(^|\n)[[:space:]]*[(]device[[:space:]]/ && block !~ /\.\.\./) {
			out=sprintf("%s/%s.%d.scm", dir, safename(file), n)
			printf "%s", block > out
			print out
		}
		next
	}
	inb { block = block $0 "\n" }
	' "$1"
}

for f in README.md docs/*.md; do
	[ -f "$f" ] || continue
	extract "$f" >> "$status.list"
done

checked=0
failed=0
while IFS= read -r example; do
	[ -n "$example" ] || continue
	checked=$((checked + 1))
	if ! "$bin" --check "$example" >/dev/null 2>"$tmpdir/err"; then
		echo "FAIL doc example: $example" >&2
		cat "$tmpdir/err" >&2
		failed=$((failed + 1))
	fi
done < "$status.list"

echo "checked $checked doc examples, $failed failed"
[ "$failed" -eq 0 ]
