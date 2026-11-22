#!/bin/sh
set -eu

# Enforce documented dependency direction:
#   net -> modbus -> device -> util
#   config -> device -> util
# Checks are per-layer so forbidden edges are reported clearly.

check_forbidden() {
	label=$1
	pattern=$2
	shift 2
	bad=$(grep -rnE "$pattern" "$@" || true)
	if [ -n "$bad" ]; then
		printf 'Forbidden dependency includes found (%s):\n' "$label" >&2
		echo "$bad" >&2
		exit 1
	fi
}

# Match the include path itself, not text after the closing quote. `(\.\./)*`
# keeps the check working for relative includes such as "../modbus/pdu.h".
check_tree() {
	root=$1
	check_forbidden \
		'config/device must not include modbus/net' \
		'^#include[[:space:]]*[<"]((\.\./)*(modbus|net)/)' \
		"$root/src/config" "$root/src/device"

	check_forbidden \
		'modbus must not include net/config' \
		'^#include[[:space:]]*[<"]((\.\./)*(net|config)/)' \
		"$root/src/modbus"

	check_forbidden \
		'util must not include net/modbus/device/config' \
		'^#include[[:space:]]*[<"]((\.\./)*(net|modbus|device|config)/)' \
		"$root/src/util"
}

# The check used to be vacuously green because the regex consumed the include
# path before looking for a forbidden directory. Keep a fixture per rule so it
# cannot silently regress again.
self_test() {
	tmp=$(mktemp -d)
	trap 'rm -rf "$tmp"' EXIT HUP INT TERM

	clean=$tmp/clean
	mkdir -p "$clean/src/config" "$clean/src/device" \
		 "$clean/src/modbus" "$clean/src/util"
	printf '#include "device/device.h"\n' >"$clean/src/config/ok.c"
	printf '#include "util/log.h"\n' >"$clean/src/modbus/ok.c"
	check_tree "$clean"

	expect_fail() {
		name=$1
		dir=$2
		include=$3

		mkdir -p "$tmp/$name/src/$dir"
		printf '#include "%s"\n' "$include" >"$tmp/$name/src/$dir/bad.c"
		if ( check_tree "$tmp/$name" ) >/dev/null 2>&1; then
			printf 'check_deps.sh self-test: %s did not fail\n' "$name" >&2
			exit 1
		fi
	}

	expect_fail config-net      config "net/net_common.h"
	expect_fail device-modbus   device "modbus/pdu.h"
	expect_fail modbus-net      modbus "net/net_common.h"
	expect_fail modbus-config   modbus "config/config.h"
	expect_fail util-device     util   "device/device.h"
	expect_fail util-relative   util   "../modbus/rtu.h"
	printf 'check_deps.sh self-test: ok\n'
}

case "${1:-}" in
--self-test) self_test ;;
"") check_tree "." ;;
*)
	printf 'usage: %s [--self-test]\n' "$0" >&2
	exit 2
	;;
esac
