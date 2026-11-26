#!/bin/sh
set -eu

# Builds the prod artifacts, release tarball and SBOM twice, in two copies of the tree
# at different absolute paths, and fails if any sha256 differs.
# Usage: tools/check_reproducible.sh [make args, e.g. WITH_TLS=0]
# Needs a git checkout with submodules (the file list comes from git); uncommitted changes
# are included.

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM

# Both builds see the same revision and epoch, as two builds of one commit would.
rev=$(git -C "$root" describe --always --dirty 2>/dev/null || echo unknown)
epoch=$(git -C "$root" log -1 --format=%ct)
export SOURCE_DATE_EPOCH=$epoch
# The copies have no .git, so the SBOM gets the vlib submodule commit from here.
VLIB_REVISION=$(git -C "$root" ls-tree HEAD vlib 2>/dev/null | awk '{print $3}')
export VLIB_REVISION

for n in a b; do
	dir=$work/$n/mogbus-build-$n
	mkdir -p "$dir"
	(cd "$root" && { git ls-files -c --recurse-submodules -z; git ls-files -o --exclude-standard -z; } | tar --null -T - -cf -) | tar -C "$dir" -xf -
	echo "building in $dir"
	# CCACHE_DISABLE keeps the comparison about the compiler, not the cache.
	(cd "$dir" && CCACHE_DISABLE=1 make -j"$(nproc)" GIT_REVISION="$rev" "$@" dist >"$work/build-$n.log" 2>&1) ||
		{ tail -30 "$work/build-$n.log" >&2; echo "build $n failed" >&2; exit 1; }
	(cd "$dir" && sha256sum mogbus libmogbus.a libmogbus.so dist/*.tar.gz dist/*.spdx.json) >"$work/sums-$n"
done

if diff -u "$work/sums-a" "$work/sums-b" >"$work/diff"; then
	cat "$work/sums-a"
	echo "reproducible: OK"
else
	cat "$work/diff" >&2
	echo "reproducible: FAILED (artifacts differ)" >&2
	exit 1
fi
