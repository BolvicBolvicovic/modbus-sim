#!/bin/sh
set -eu

# `make install` into a temporary DESTDIR, then builds examples/embed_tcp.c against the installed
# files with `pkg-config --cflags --libs mogbus`, once against the shared and once against the
# static library, runs both, and checks that `make uninstall` leaves no file behind.
# Run by `make lib-check` (MAKE and CC come from the Makefile); skipped without pkg-config.

root=$(cd "$(dirname "$0")/.." && pwd)
make=${MAKE:-make}
cc=${CC:-cc}
prefix=/opt/mogbus-check

command -v pkg-config >/dev/null 2>&1 || { echo "check_install: skipped (no pkg-config)"; exit 0; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM
dest=$work/root
root_prefix=$dest$prefix

cd "$root"
$make --no-print-directory install DESTDIR="$dest" PREFIX="$prefix" >"$work/install.log" 2>&1 ||
	{ cat "$work/install.log" >&2; echo "check_install: make install failed" >&2; exit 1; }

for f in lib/libmogbus.a lib/libmogbus.so lib/pkgconfig/mogbus.pc include/mogbus.h \
	share/doc/mogbus/README.md share/doc/mogbus/docs/building.md; do
	[ -f "$root_prefix/$f" ] || { echo "check_install: $f was not installed" >&2; exit 1; }
done
if [ -f "$root_prefix/bin/mogbus" ]; then
	"$root_prefix/bin/mogbus" --version >/dev/null
	"$root_prefix/bin/mogbus" --check "$root_prefix/share/mogbus/profiles/minimal.scm"
fi

# The sysroot makes pkg-config prepend the temporary DESTDIR to every -I and -L path.
export PKG_CONFIG_PATH=$root_prefix/lib/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$dest
header_version=$(awk '/^#define MOGBUS_VERSION_(MAJOR|MINOR|PATCH)/ { v = v (v ? "." : "") $3 } END { print v }' include/mogbus.h)
[ "$(pkg-config --modversion mogbus)" = "$header_version" ] ||
	{ echo "check_install: mogbus.pc has version $(pkg-config --modversion mogbus), not $header_version" >&2; exit 1; }
cflags=$(pkg-config --cflags mogbus)

# Shared: the library is found at run time through LD_LIBRARY_PATH.
# shellcheck disable=SC2086
$cc $cflags examples/embed_tcp.c $(pkg-config --libs mogbus) -o "$work/embed_shared"
LD_LIBRARY_PATH=$root_prefix/lib "$work/embed_shared" >/dev/null

# Static: --static adds Libs.private (and the OpenSSL requirement of a TLS build).
static_libs=$(pkg-config --static --libs mogbus | sed 's/-lmogbus/-Wl,-Bstatic -lmogbus -Wl,-Bdynamic/')
# shellcheck disable=SC2086
$cc $cflags examples/embed_tcp.c $static_libs -o "$work/embed_static"
"$work/embed_static" >/dev/null

$make --no-print-directory uninstall DESTDIR="$dest" PREFIX="$prefix" >"$work/uninstall.log" 2>&1 ||
	{ cat "$work/uninstall.log" >&2; echo "check_install: make uninstall failed" >&2; exit 1; }
left=$(find "$dest" -type f)
[ -z "$left" ] || { echo "check_install: make uninstall left files behind:" >&2; echo "$left" >&2; exit 1; }
echo "check_install: ok"
