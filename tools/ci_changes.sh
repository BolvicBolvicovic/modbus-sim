#!/bin/sh
set -eu

# Decides which CI jobs a change needs, so comment-only and packaging-only pushes do not
# run the build and test jobs. Prints code=, docs= and docker= lines (true/false), for
# $GITHUB_OUTPUT. Usage: tools/ci_changes.sh BASE HEAD (an empty or all-zero BASE, e.g.
# a new branch or workflow_dispatch, means "everything changed").
#   code   - a C file changed outside comments, or any other build/test input changed
#   docs   - a C file or Doxyfile changed (Doxygen comments may need checking)
#   docker - code changed, or the Dockerfile, .dockerignore or deploy/ changed
# Files that CI does not watch (docs, README, LICENSE, ...) never set anything.

base=${1:-}
head=${2:-HEAD}
code=false
docs=false
docker=false

emit() {
	printf 'code=%s\ndocs=%s\ndocker=%s\n' "$code" "$docs" "$docker"
	exit 0
}

case $base in
''|*[!0]*) ;;
*) base='' ;;
esac
if [ -z "$base" ] || ! git cat-file -e "$base^{commit}" 2>/dev/null; then
	code=true docs=true docker=true
	emit
fi

# Comments and whitespace removed: a C file whose stripped text is unchanged only had
# comment edits.
strip_c() {
	gcc -fpreprocessed -dD -E -P -x c - 2>/dev/null | tr -d ' \t\n'
}

for f in $(git diff --name-only "$base" "$head"); do
	case $f in
	src/*.c|src/*.h|include/*.h|examples/*.c|tests/*.c|tests/*.h)
		docs=true
		old=$(git show "$base:$f" 2>/dev/null | strip_c || true)
		new=$(git show "$head:$f" 2>/dev/null | strip_c || true)
		[ "$old" = "$new" ] || code=true
		;;
	Doxyfile)
		docs=true ;;
	Dockerfile|.dockerignore|deploy/*)
		docker=true ;;
	src/*|include/*|examples/*|tests/*|tools/*|vlib/*|third_party/*|profiles/*|python/*|Makefile|.github/workflows/ci.yml)
		code=true ;;
	esac
done
if [ "$code" = true ]; then
	docs=true
	docker=true
fi
emit
