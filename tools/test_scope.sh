#!/bin/sh
# Resolves the WHAT value of `make tests` into scopes (c, python, docs).
# usage: test_scope.sh WHAT      prints the scopes (space-separated) on stdout and a one-line
#                                explanation on stderr; exits 1 on an invalid WHAT
#        test_scope.sh --self-test
# WHAT is a comma-separated list of c, python, docs, all, auto. `auto` looks at the files changed
# versus the merge base with origin/main plus uncommitted and untracked ones; without git, origin
# or any recognised change it falls back to c.
set -u

# Reads paths on stdin, prints "scope path" lines for every scope a path belongs to.
classify() {
	while IFS= read -r f; do
		case "$f" in
		src/*|tests/*|include/*|profiles/*|Makefile|vlib|vlib/*|.gitmodules|third_party/s7/*|\
		examples/*.c|tools/run_tests.sh|tools/check_toolchain.sh|tools/check_deps.sh|\
		tools/check_reproducible.sh|tools/gen_sbom.sh|tools/run_fuzz.sh|tools/fuzz_seeds.py)
			printf 'c %s\n' "$f" ;;
		esac
		case "$f" in
		python/*|tools/mogbus_import.py|examples/python/*|examples/import/*|include/mogbus.h)
			printf 'python %s\n' "$f" ;;
		esac
		case "$f" in
		*.md|docs/*|Doxyfile|tools/docs/*|tools/check_doc_briefs.sh|tools/check_doc_examples.sh|\
		third_party/doxygen-awesome-css/*)
			printf 'docs %s\n' "$f" ;;
		esac
	done
}

# Reads classified lines on stdin, prints "<scopes>\t<message>" for auto mode.
summarize() {
	lines=$(cat)
	scopes=''
	detail=''
	for s in c python docs; do
		first=$(printf '%s\n' "$lines" | awk -v s="$s" '$1 == s { print $2 }' | head -n 2 | paste -sd, - | sed 's/,/, /')
		if [ -n "$first" ]; then
			scopes=${scopes:+$scopes,}$s
			detail="${detail:+$detail; }$s: $first"
		fi
	done
	if [ -z "$scopes" ]; then
		printf 'c\ttests: auto -> c (no recognised changes)\n'
	else
		printf '%s\ttests: auto -> %s (%s)\n' "$scopes" "$scopes" "$detail"
	fi
}

changed_files() {
	git rev-parse --git-dir >/dev/null 2>&1 || return 1
	base=$(git merge-base HEAD origin/main 2>/dev/null) || return 1
	{
		git diff --name-only "$base" --
		git diff --name-only HEAD --
		git ls-files -o --exclude-standard
	} | sort -u
}

self_test() {
	out=$(printf 'src/a.c\npython/x.py\n' | classify | summarize)
	exp=$(printf 'c,python\ttests: auto -> c,python (c: src/a.c; python: python/x.py)')
	[ "$out" = "$exp" ] || { printf 'test_scope.sh self-test: got %s\n' "$out" >&2; exit 1; }
	out=$(printf 'include/mogbus.h\n' | classify | summarize | cut -f1)
	[ "$out" = "c,python" ] || { printf 'test_scope.sh self-test: mogbus.h -> %s\n' "$out" >&2; exit 1; }
	out=$(printf 'README.md\ntools/docs/build.py\n' | classify | summarize | cut -f1)
	[ "$out" = "docs" ] || { printf 'test_scope.sh self-test: docs -> %s\n' "$out" >&2; exit 1; }
	out=$(printf '.github/workflows/ci.yml\n' | classify | summarize | cut -f1)
	[ "$out" = "c" ] || { printf 'test_scope.sh self-test: fallback -> %s\n' "$out" >&2; exit 1; }
	printf 'test_scope.sh self-test: ok\n'
}

case "${1:-}" in
--self-test) self_test; exit 0 ;;
''|-h|--help) printf 'usage: %s WHAT | --self-test\n' "$0" >&2; exit 2 ;;
esac

what=$1
valid='c, python, docs, all, auto'
scopes=''
auto=0
oldifs=$IFS
IFS=,
for w in $what; do
	case "$w" in
	c|python|docs) scopes="$scopes $w" ;;
	all) scopes="$scopes c python docs" ;;
	auto) auto=1 ;;
	*) printf 'make tests: unknown WHAT value "%s" (valid: %s)\n' "$w" "$valid" >&2; exit 1 ;;
	esac
done
IFS=$oldifs

msg=
if [ "$auto" -eq 1 ]; then
	if files=$(changed_files); then
		res=$(printf '%s\n' "$files" | classify | summarize)
	else
		res=$(printf 'c\ttests: auto -> c (git or origin/main unavailable)\n')
	fi
	scopes="$scopes $(printf '%s' "$res" | cut -f1 | tr ',' ' ')"
	msg=$(printf '%s' "$res" | cut -f2)
fi

if [ -z "$scopes" ]; then
	printf 'make tests: empty WHAT (valid: %s)\n' "$valid" >&2
	exit 1
fi
out=''
for s in c python docs; do
	case " $scopes " in *" $s "*) out=${out:+$out,}$s ;; esac
done
[ -n "$msg" ] || msg="tests: WHAT=$what -> $out"
printf '%s\n' "$msg" >&2
printf '%s\n' "$out" | tr ',' ' '
