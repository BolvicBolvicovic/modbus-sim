#!/bin/sh
# Builds the reference-template fixtures (tools/docs/fixtures) into build/docs-fixtures. They are
# never part of the real site. Needs doxygen and python3; run via make docs-fixtures.
set -eu

cd "$(dirname "$0")/../.."
rm -rf build/docs-fixtures
mkdir -p build/docs-fixtures
{
	cat Doxyfile
	echo "INPUT = tools/docs/fixtures"
	echo "EXCLUDE_PATTERNS ="
	echo "USE_MDFILE_AS_MAINPAGE ="
	echo "OUTPUT_DIRECTORY = build/docs-fixtures"
	echo "GENERATE_HTML = NO"
	echo "XML_PROGRAMLISTING = NO"
} | doxygen -
python3 tools/docs/build.py --xml build/docs-fixtures/xml --out build/docs-fixtures \
	--manifest tools/docs/fixtures/site.json --no-internals
