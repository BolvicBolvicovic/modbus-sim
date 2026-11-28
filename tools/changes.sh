#!/bin/sh
# Maps changed files to what has to run, for CI and for `make tests`. One table (tags) decides
# which categories a path belongs to, so the two modes cannot drift.
#
#   changes.sh ci BASE HEAD     Decides which CI jobs a change needs, so comment-only and
#                               packaging-only pushes do not run the build and test jobs. Prints
#                               code=, docs= and docker= lines (true/false) for $GITHUB_OUTPUT. An
#                               empty or all-zero BASE (a new branch, workflow_dispatch) means
#                               "everything changed". Files CI does not watch (LICENSE, ...) never
#                               set anything.
#                                 code   - a C file changed outside comments, or any other
#                                          build/test input changed
#                                 docs   - a C file, Doxyfile, Markdown page, docs/ file, the
#                                          Doxygen theme or the site generator (tools/docs/)
#                                          changed (the docs job builds all of them)
#                                 docker - code changed, or the Dockerfile, .dockerignore or
#                                          deploy/ changed
#   changes.sh scope WHAT       Resolves the WHAT value of `make tests` into scopes (c, python,
#                               docs): prints them (space-separated) on stdout and a one-line
#                               explanation on stderr; exits 1 on an invalid WHAT. WHAT is a
#                               comma-separated list of c, python, docs, all, auto. `auto` looks
#                               at the files changed versus the merge base with origin/main plus
#                               uncommitted and untracked ones; without git, origin or any
#                               recognised change it falls back to c.
#   changes.sh --self-test
set -u

# Prints the categories of path $1. Scopes of `scope`: c, python, docs. Inputs of `ci`: cfile (C
# source, code only when more than comments changed), site (docs job only), docker, code.
tags() {
	t=''
	case $1 in
	src/*|tests/*|include/*|profiles/*|Makefile|vlib|vlib/*|.gitmodules|third_party/s7/*|\
	examples/*.c|tools/run_tests.sh|tools/check_toolchain.sh|tools/check_deps.sh|\
	tools/check_reproducible.sh|tools/check_install.sh|tools/gen_sbom.sh|tools/fuzz.py)
		t="$t c" ;;
	esac
	case $1 in
	python/*|tools/mogbus_import.py|examples/python/*|examples/import/*|include/mogbus.h)
		t="$t python" ;;
	esac
	case $1 in
	*.md|docs/*|Doxyfile|tools/docs/*|tools/check_docs.sh|third_party/doxygen-awesome-css/*)
		t="$t docs" ;;
	esac
	case $1 in
	src/*.c|src/*.h|include/*.h|examples/*.c|tests/*.c|tests/*.h)
		t="$t cfile" ;;
	Doxyfile|README.md|CONTRIBUTING.md|SECURITY.md|NOTICE.md|AGENTS.md|docs/*|\
	third_party/doxygen-awesome-css/*|tools/docs/*)
		t="$t site" ;;
	Dockerfile|.dockerignore|deploy/*)
		t="$t docker" ;;
	src/*|include/*|examples/*|tests/*|tools/*|vlib|vlib/*|.gitmodules|third_party/*|profiles/*|python/*|Makefile|.github/workflows/ci.yml)
		t="$t code" ;;
	esac
	printf '%s\n' "$t"
}

# Comments and whitespace removed: a C file whose stripped text is unchanged only had comment edits.
strip_c() {
	gcc -fpreprocessed -dD -E -P -x c - 2>/dev/null | tr -d ' \t\n'
}

# Reads changed paths on stdin (BASE is $1, HEAD $2), prints the code=, docs= and docker= lines.
ci_result() {
	code=false docs=false docker=false
	while IFS= read -r f; do
		t=$(tags "$f")
		case " $t " in
		*" cfile "*)
			docs=true
			old=$(git show "$1:$f" 2>/dev/null | strip_c || true)
			new=$(git show "$2:$f" 2>/dev/null | strip_c || true)
			[ "$old" = "$new" ] || code=true ;;
		*" site "*) docs=true ;;
		*" docker "*) docker=true ;;
		*" code "*) code=true ;;
		esac
	done
	if [ "$code" = true ]; then
		docs=true
		docker=true
	fi
	printf 'code=%s\ndocs=%s\ndocker=%s\n' "$code" "$docs" "$docker"
}

ci() {
	base=${1:-}
	head=${2:-HEAD}
	case $base in
	''|*[!0]*) ;;
	*) base='' ;;
	esac
	if [ -z "$base" ] || ! git cat-file -e "$base^{commit}" 2>/dev/null; then
		printf 'code=true\ndocs=true\ndocker=true\n'
		return
	fi
	git diff --name-only "$base" "$head" | ci_result "$base" "$head"
}

# Reads paths on stdin, prints "scope path" lines for every scope a path belongs to.
classify() {
	while IFS= read -r f; do
		t=$(tags "$f")
		for s in c python docs; do
			case " $t " in *" $s "*) printf '%s %s\n' "$s" "$f" ;; esac
		done
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

scope() {
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
		*) printf 'make tests: unknown WHAT value "%s" (valid: %s)\n' "$w" "$valid" >&2; return 1 ;;
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
		return 1
	fi
	out=''
	for s in c python docs; do
		case " $scopes " in *" $s "*) out=${out:+$out,}$s ;; esac
	done
	[ -n "$msg" ] || msg="tests: WHAT=$what -> $out"
	printf '%s\n' "$msg" >&2
	printf '%s\n' "$out" | tr ',' ' '
}

self_test() {
	fail() { printf 'changes.sh self-test: %s\n' "$1" >&2; exit 1; }
	out=$(printf 'src/a.c\npython/x.py\n' | classify | summarize)
	exp=$(printf 'c,python\ttests: auto -> c,python (c: src/a.c; python: python/x.py)')
	[ "$out" = "$exp" ] || fail "scope got $out"
	out=$(printf 'include/mogbus.h\n' | classify | summarize | cut -f1)
	[ "$out" = "c,python" ] || fail "mogbus.h -> $out"
	out=$(printf 'README.md\ntools/docs/build.py\n' | classify | summarize | cut -f1)
	[ "$out" = "docs" ] || fail "docs -> $out"
	out=$(printf '.github/workflows/ci.yml\n' | classify | summarize | cut -f1)
	[ "$out" = "c" ] || fail "fallback -> $out"

	# CI mode in a scratch repository.
	here=$(cd "$(dirname "$0")" && pwd)
	tmp=$(mktemp -d "${TMPDIR:-/tmp}/mogbus-changes.XXXXXX")
	trap 'rm -rf "$tmp"' EXIT HUP INT TERM
	(
		cd "$tmp" || exit 1
		git init -q . || exit 1
		git config user.email t@t
		git config user.name t
		mkdir src docs
		printf 'int f(void) { return 1; }\n' >src/a.c
		printf 'x\n' >LICENSE
		git add -A
		git commit -qm base
	) || fail "scratch repository"
	check() { # NAME EXPECTED (code docs docker)
		got=$(cd "$tmp" && sh "$here/changes.sh" ci "$base" HEAD | tr '\n' ' ')
		[ "$got" = "$2" ] || fail "ci $1: got $got, want $2"
	}
	commit() { (cd "$tmp" && git add -A && git commit -qm "$1" >/dev/null); }
	base=$(cd "$tmp" && git rev-parse HEAD)
	printf '/* new comment */\nint f(void) { return 1; } // note\n' >"$tmp/src/a.c"
	commit comment
	check "comment-only C" 'code=false docs=true docker=false '
	base=$(cd "$tmp" && git rev-parse HEAD)
	printf 'int f(void) { return 2; }\n' >"$tmp/src/a.c"
	commit code
	check "C code" 'code=true docs=true docker=true '
	base=$(cd "$tmp" && git rev-parse HEAD)
	printf 'FROM x\n' >"$tmp/Dockerfile"
	commit docker
	check "Dockerfile" 'code=false docs=false docker=true '
	base=$(cd "$tmp" && git rev-parse HEAD)
	printf 'y\n' >"$tmp/LICENSE"
	printf 'p\n' >"$tmp/docs/p.md"
	commit docs
	check "docs" 'code=false docs=true docker=false '
	base=$(cd "$tmp" && git rev-parse HEAD)
	printf 'z\n' >"$tmp/LICENSE"
	commit license
	check "unwatched" 'code=false docs=false docker=false '
	got=$(cd "$tmp" && sh "$here/changes.sh" ci 0000000000000000000000000000000000000000 HEAD | tr '\n' ' ')
	[ "$got" = 'code=true docs=true docker=true ' ] || fail "ci all-zero base: $got"
	printf 'changes.sh self-test: ok\n'
}

case "${1:-}" in
--self-test) self_test; exit 0 ;;
ci) shift; ci "$@" ;;
scope) [ $# -eq 2 ] || { printf 'usage: %s scope WHAT\n' "$0" >&2; exit 2; }; scope "$2" ;;
*) printf 'usage: %s ci BASE HEAD | scope WHAT | --self-test\n' "$0" >&2; exit 2 ;;
esac
