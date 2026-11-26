#!/bin/sh
# Fails when a file, struct, union, enum or opaque/function-pointer typedef has no Doxygen brief.
# Runs Doxygen with XML output only (build/api/xml) and reads the result with python3.
set -eu

cd "$(dirname "$0")/.."
out=build/api/xml

rm -rf "$out"
mkdir -p build/api
{
	cat Doxyfile
	echo "GENERATE_HTML = NO"
	echo "GENERATE_XML = YES"
	echo "XML_OUTPUT = xml"
	echo "XML_PROGRAMLISTING = NO"
} | doxygen - >/dev/null

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
