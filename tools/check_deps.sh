#!/bin/sh
set -eu

# Enforce documented dependency direction:
#   net -> modbus -> device -> util
#   config -> device -> util
#
# Any internal module include not explicitly allowed is rejected.
# net->util and modbus->util are allowed as existing practical edges;
# net->device/config and modbus->config remain forbidden.

check_layer() {
	root=$1
	layer=$2
	allowed=$3
	dir=$root/src/$layer
	[ -d "$dir" ] || return 0

	bad=$(grep -rnE \
		'^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]((\.\./)*(net|modbus|device|config|util)/[^">]+)[">]' \
		"$dir" 2>/dev/null || true)
	[ -n "$bad" ] || return 0

	fail=0
	old_ifs=$IFS
	IFS='
'
	for line in $bad; do
		IFS=$old_ifs
		[ -n "$line" ] || continue
		include=$(printf '%s\n' "$line" | sed -nE \
			's!.*#[[:space:]]*include[[:space:]]*[<"](((\.\./)*(net|modbus|device|config|util)/[^">]+))[">].*!\1!p')
		target=$(printf '%s\n' "$include" | sed -E 's!^(\.\./)*!!; s!/.*!!')
		case " $allowed " in
		*" $target "*) ;;
		*)
			printf 'Forbidden dependency includes found (%s -> %s):\n' \
				"$layer" "$target" >&2
			printf '%s\n' "$line" >&2
			fail=1
			;;
		esac
	done
	IFS=$old_ifs
	[ "$fail" -eq 0 ] || exit 1
}

check_file_forbidden_include() {
	file=$1
	pattern=$2
	reason=$3
	[ -f "$file" ] || return 0
	bad=$(grep -nE "$pattern" "$file" 2>/dev/null || true)
	[ -n "$bad" ] || return 0
	printf 'Forbidden dependency include found (%s):\n' "$reason" >&2
	printf '%s:%s\n' "$file" "$bad" >&2
	exit 1
}

check_extra_rules() {
	root=$1
	# Issue 50: RTU timing/framing helpers live in rtu_framer; the TCP RTU server
	# is a peer of the byte-stream RTU server, not its client.
	check_file_forbidden_include "$root/src/net/rtu_tcp_server.c" 		'^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]((\.\./)*)net/rtu_server\.h[">]' 		"rtu_tcp_server.c must not include rtu_server.h"
}

check_tree() {
	root=$1
	check_layer "$root" net    "net modbus util"
	check_layer "$root" modbus "modbus device util"
	check_layer "$root" device "device util"
	check_layer "$root" config "config device util"
	check_layer "$root" util   "util"
	check_extra_rules "$root"
}

self_test() {
	tmp=$(mktemp -d)
	trap 'rm -rf "$tmp"' EXIT HUP INT TERM

	clean=$tmp/clean
	mkdir -p "$clean/src/config" "$clean/src/device" \
		 "$clean/src/modbus" "$clean/src/net" "$clean/src/util"
	printf '#include "device/device.h"\n' >"$clean/src/config/ok.c"
	printf '#include "util/log.h"\n'       >"$clean/src/config/ok-util.c"
	printf '#include "util/log.h"\n'       >"$clean/src/device/ok.c"
	printf '#include "device/device.h"\n'  >"$clean/src/modbus/ok.c"
	printf '#include "util/log.h"\n'       >"$clean/src/modbus/ok-util.c"
	printf '#include "modbus/pdu.h"\n'     >"$clean/src/net/ok.c"
	printf '#include "util/log.h"\n'       >"$clean/src/net/ok-util.c"
	printf '#include "util/rng.h"\n'       >"$clean/src/util/ok.c"
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

	# Issue 36 acceptance fixtures.
	expect_fail net-device      net    "device/device.h"
	expect_fail modbus-config   modbus "config/config.h"

	expect_fail config-net      config "net/net_common.h"
	expect_fail config-modbus   config "modbus/pdu.h"
	expect_fail device-config   device "config/config.h"
	expect_fail device-modbus   device "modbus/pdu.h"
	expect_fail device-net      device "net/net_common.h"
	expect_fail modbus-net      modbus "net/net_common.h"
	expect_fail net-config      net    "config/config.h"
	expect_fail util-device     util   "device/device.h"
	expect_fail util-net        util   "net/net_common.h"
	expect_fail util-modbus     util   "modbus/pdu.h"
	expect_fail util-config     util   "config/config.h"
	expect_fail util-relative   util   "../modbus/rtu.h"

	# Issue 50 acceptance fixture.
	mkdir -p "$tmp/rtu-tcp-server-peer/src/net"
	printf '#include "net/rtu_server.h"\n' >"$tmp/rtu-tcp-server-peer/src/net/rtu_tcp_server.c"
	if ( check_tree "$tmp/rtu-tcp-server-peer" ) >/dev/null 2>&1; then
		printf 'check_deps.sh self-test: rtu-tcp-server-peer did not fail\n' >&2
		exit 1
	fi
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
