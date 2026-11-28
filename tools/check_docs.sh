#!/bin/sh
# Documentation checks, run from the repository root.
#   check_docs.sh briefs        fails when a file, struct, union, enum or opaque/function-pointer
#                               typedef has no Doxygen brief (reads the XML `doxygen Doxyfile`
#                               wrote to build/docs/xml with python3; make docs runs it first)
#   check_docs.sh examples BIN  extracts fenced ```scheme blocks from README.md and docs/ and runs
#                               BIN --check on blocks that look like complete configs (contain a
#                               top-level (device ...) form)
set -eu

cd "$(dirname "$0")/.."

briefs() {
	out=build/docs/xml
	[ -d "$out" ] || { echo "check_docs: $out missing (run doxygen Doxyfile first)" >&2; exit 1; }

python3 -I - "$out" <<'EOF'
import glob, os, sys
import xml.etree.ElementTree as ET

out = sys.argv[1]
missing = []
structs = set()      # names of struct/union compounds and named enums that exist
typedefs = []        # (name, type text, location, has brief)

def has_brief(node):
    b = node.find('briefdescription')
    return b is not None and ''.join(b.itertext()).strip() != ''

def loc(node):
    l = node.find('location')
    return '%s:%s' % (l.get('file'), l.get('line')) if l is not None else '?'

for path in sorted(glob.glob(os.path.join(out, '*.xml'))):
    if os.path.basename(path) == 'index.xml':
        continue
    root = ET.parse(path).getroot()
    for c in root.iter('compounddef'):
        kind = c.get('kind')
        name = c.findtext('compoundname') or ''
        if kind in ('struct', 'union'):
            if '@' in name:
                continue
            structs.add(name)
            if not has_brief(c):
                missing.append((loc(c), '%s %s' % (kind, name)))
        elif kind == 'file' and name.endswith(('.c', '.h')):
            if not has_brief(c):
                missing.append((loc(c), 'file %s' % name))
        for m in c.iter('memberdef'):
            mk = m.get('kind')
            mname = m.findtext('name') or ''
            if mk == 'enum':
                if mname.startswith('@'):
                    mname = '(anonymous enum)'
                else:
                    structs.add(mname)
                if not has_brief(m):
                    missing.append((loc(m), 'enum %s' % mname))
            elif mk == 'typedef':
                typedefs.append((mname, ''.join(m.find('type').itertext()).strip(), loc(m), has_brief(m)))

for name, ty, where, brief in typedefs:
    if brief:
        continue
    words = ty.split()
    # typedef struct x x / typedef enum x x: the definition carries the brief
    if len(words) == 2 and words[0] in ('struct', 'enum', 'union') and words[1] in structs:
        continue
    # an alias of an already documented type needs nothing more
    if len(words) == 1 and words[0] in structs:
        continue
    missing.append((where, 'typedef %s' % name))

seen = set()
for where, what in sorted(set(missing)):
    print('%s: no brief description for %s' % (where, what))
if missing:
    sys.exit(1)
EOF
}

examples() {
	bin=${1:-./mogbus}
	tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/mogbus-doccheck.XXXXXX")
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

	for f in README.md $(find docs -name '*.md' | sort); do
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
}

case "${1:-}" in
briefs) briefs ;;
examples) examples "${2:-./mogbus}" ;;
*) echo "usage: $0 briefs | examples BIN" >&2; exit 2 ;;
esac
