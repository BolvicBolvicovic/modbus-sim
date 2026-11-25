#!/bin/sh
# Verify Makefile toolchain selection with an isolated PATH: only the shimmed
# tools exist, so each scenario exercises one combination independently.
set -eu

# Inherited make variables would mask the defaults under test.
unset CC LDFLAGS MAKEFLAGS MFLAGS GNUMAKEFLAGS 2>/dev/null || true

make_bin=$(command -v make)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/toolchain.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
bin=$tmp/bin
mkdir "$bin"

# Tools needed while make parses the Makefile; real compilers stay off PATH.
for tool in sh find pkg-config; do
	path=$(command -v "$tool" 2>/dev/null || true)
	if [ -n "$path" ]; then
		ln -s "$path" "$bin/$tool"
	fi
done

# A compiler shim that accepts any probe flags. make -n never runs recipes with
# it, only the one-line mold probe through $(shell ...).
printf '#!/bin/sh\nexit 0\n' >"$bin/compiler"
printf '#!/bin/sh\ncase " $* " in *" -fuse-ld=mold "*) exit 1 ;; esac\nexit 0\n' >"$bin/old-compiler"
printf '#!/bin/sh\nexec "$@"\n' >"$bin/forward-ccache"
chmod +x "$bin/compiler" "$bin/old-compiler" "$bin/forward-ccache"

clear_shims() {
	rm -f "$bin/clang" "$bin/gcc" "$bin/cc" "$bin/ccache" "$bin/mold"
}

shim() {
	for name in "$@"; do
		ln -s compiler "$bin/$name"
	done
}

expect() {
	what=$1
	mode=$2
	pattern=$3
	output=$4
	if [ "$mode" = match ]; then
		if ! printf '%s\n' "$output" | grep -qE -- "$pattern"; then
			printf 'check_toolchain.sh: %s (expected /%s/)\n' "$what" "$pattern" >&2
			printf '%s\n' "$output" >&2
			exit 1
		fi
	else
		if printf '%s\n' "$output" | grep -qE -- "$pattern"; then
			printf 'check_toolchain.sh: %s (unexpected /%s/)\n' "$what" "$pattern" >&2
			printf '%s\n' "$output" >&2
			exit 1
		fi
	fi
}

clear_shims
shim clang
out=$(PATH="$bin" "$make_bin" -n 2>&1)
expect 'clang-only selects clang' match '^clang ' "$out"
expect 'clang-only skips ccache' reject '^ccache ' "$out"
expect 'clang-only uses the default linker' reject '-fuse-ld=mold' "$out"

clear_shims
shim gcc
out=$(PATH="$bin" "$make_bin" -n 2>&1)
expect 'gcc-only selects gcc' match '^gcc ' "$out"

clear_shims
shim cc
out=$(PATH="$bin" "$make_bin" -n 2>&1)
expect 'cc-only selects cc' match '^cc ' "$out"

clear_shims
shim clang ccache mold
out=$(PATH="$bin" "$make_bin" -n 2>&1)
expect 'ccache prefixes clang' match '^ccache clang ' "$out"
expect 'mold is used when accepted' match '-fuse-ld=mold' "$out"

clear_shims
shim mold
ln -s old-compiler "$bin/gcc"
out=$(PATH="$bin" "$make_bin" -n 2>&1)
expect 'compiler rejecting mold keeps default linker' reject '-fuse-ld=mold' "$out"

clear_shims
shim mold
ln -s old-compiler "$bin/clang"
ln -s forward-ccache "$bin/ccache"
out=$(PATH="$bin" "$make_bin" -n 2>&1)
expect 'ccache prefixes the rejecting compiler' match '^ccache clang ' "$out"
expect 'ccache-prefixed probe rejects mold' reject '-fuse-ld=mold' "$out"

clear_shims
shim clang ccache mold gcc
out=$(PATH="$bin" CC=gcc "$make_bin" -n 2>&1)
expect 'explicit CC wins' match '^gcc ' "$out"
expect 'explicit CC is not wrapped in ccache' reject '^ccache ' "$out"

out=$(PATH="$bin" LDFLAGS=-Wl,--as-needed "$make_bin" -n 2>&1)
expect 'explicit LDFLAGS are used with hardening flags' match '-Wl,--as-needed -pie -Wl,-z,relro,-z,now' "$out"
expect 'explicit LDFLAGS wins' reject '-fuse-ld=mold' "$out"

printf 'check_toolchain.sh: ok\n'
