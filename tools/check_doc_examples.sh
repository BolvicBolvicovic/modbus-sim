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

# Detect a no-TLS binary without depending on the example certificate paths.
# A TLS build fails because the probe certificate does not exist; a no-TLS
# build fails earlier in config loading with the explicit message.
tls_probe=$tmpdir/tls-probe.scm
cat >"$tls_probe" <<'EOF'
(server (mode tcp)
  (tls (certificate "no-such-cert.pem") (private-key "no-such-key.pem")
       (require-client-certificate off)))
(device (unit-id 1) (holding 0 (static 1)))
EOF
"$bin" --check "$tls_probe" >/dev/null 2>"$tmpdir/tls-probe.err" || true
if grep -q 'built without TLS support' "$tmpdir/tls-probe.err"; then
	tls_supported=0
else
	tls_supported=1
fi

checked=0
skipped=0
failed=0
while IFS= read -r example; do
	[ -n "$example" ] || continue
	if [ "$tls_supported" -eq 0 ] && grep -q '(tls' "$example"; then
		skipped=$((skipped + 1))
		continue
	fi
	checked=$((checked + 1))
	if ! "$bin" --check "$example" >/dev/null 2>"$tmpdir/err"; then
		echo "FAIL doc example: $example" >&2
		cat "$tmpdir/err" >&2
		failed=$((failed + 1))
	fi
done < "$status.list"

echo "checked $checked doc examples, $failed failed"
[ "$skipped" -eq 0 ] || echo "skipped $skipped TLS examples (binary built without TLS support)"
if [ "$checked" -eq 0 ]; then
	echo "FAIL: no doc examples were checked" >&2
	exit 1
fi
[ "$failed" -eq 0 ]
