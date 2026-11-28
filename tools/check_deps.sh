#!/bin/sh
set -eu

# Enforce documented dependency direction:
#   net -> modbus -> device -> util -> os
#   net -> webui (a static page; webui includes only util and os)
#   config -> device -> util
#   server -> net, config -> server (server.h only)
#   client -> modbus, util, net/net_tls.h (a master over the wire; it knows no server, device or other net header)
#   view -> device, util, os (the live register view; api -> view)
#   api -> client, config, device, modbus, net, server, view, util
#   main.c -> mogbus.h only (the binary is a pure client of the public API)
# Only src/server/server_config.{c,h} may use config/, and config/ may use only
# server.h from server/ (the transport enum and the limits; server.h includes net/ headers
# itself, so config sees them transitively, but this check looks at direct includes only), so
# WITH_CONFIG=0 leaves out config/ and server_config.c and nothing else changes.
#
# os/ is the bottom layer and the only place in src/ that may include OS headers (see
# check_os_headers); every layer may include os/, and os/ includes no other module.
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
		'^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]((\.\./)*(net|modbus|device|config|server|api|client|util|os|webui|view)/[^">]+)[">]' \
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
			's!.*#[[:space:]]*include[[:space:]]*[<"](((\.\./)*(net|modbus|device|config|server|api|client|util|os|webui|view)/[^">]+))[">].*!\1!p')
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

check_dir_forbidden_include() {
	dir=$1
	pattern=$2
	reason=$3
	exclude=${4:-}
	[ -d "$dir" ] || return 0
	bad=$(grep -rnE ${exclude:+--exclude="$exclude"} "$pattern" "$dir" 2>/dev/null || true)
	[ -n "$bad" ] || return 0
	printf 'Forbidden dependency include found (%s):\n' "$reason" >&2
	printf '%s\n' "$bad" >&2
	exit 1
}

check_extra_rules() {
	root=$1
	# Issue 50: RTU timing/framing helpers live in rtu_framer; the TCP RTU server
	# is a peer of the byte-stream RTU server, not its client.
	check_file_forbidden_include "$root/src/net/rtu_tcp_server.c" 		'^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]((\.\./)*)net/rtu_server\.h[">]' 		"rtu_tcp_server.c must not include rtu_server.h"
	check_dir_forbidden_include "$root/src/server" \
		'^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]((\.\./)*)config/' \
		"only server_config.{c,h} may include config/" 'server_config.*'
	check_dir_forbidden_include "$root/src/config" \
		'^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]((\.\./)*)server/server_config\.h[">]' \
		"config/ may include only server.h from server/"
	# The client talks TLS through net_tls (and nothing else of net/).
	bad=$(grep -rnE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]((\.\./)*)net/' "$root/src/client" 2>/dev/null |
		grep -v 'net/net_tls\.h' || true)
	if [ -n "$bad" ]; then
		printf 'Forbidden dependency include found (client/ may include only net/net_tls.h from net/):\n' >&2
		printf '%s\n' "$bad" >&2
		exit 1
	fi
}

# Headers that belong to the OS: only src/os/ may include them (ISO C headers such as stdio.h,
# string.h, stdint.h, math.h and time.h stay allowed everywhere; errno.h is not, errors are
# classified by os_error.h).
OS_HEADERS='poll|errno|signal|fcntl|unistd|pty|termios|netdb|strings|sched|pthread|dirent|dlfcn|sys/[a-z_]+|netinet/[a-z_]+|arpa/[a-z_]+|net/[a-z_]+'

check_os_headers() {
	root=$1
	[ -d "$root/src" ] || return 0
	bad=$(grep -rnE --exclude-dir=os \
		"^[[:space:]]*#[[:space:]]*include[[:space:]]*<($OS_HEADERS)\\.h>" "$root/src" 2>/dev/null || true)
	[ -n "$bad" ] || return 0
	printf 'Forbidden OS header include found (only src/os/ may include OS headers):\n' >&2
	printf '%s\n' "$bad" >&2
	exit 1
}

check_main() {
	root=$1
	file=$root/src/main.c
	[ -f "$file" ] || return 0
	bad=$(grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]*"' "$file" 2>/dev/null |
		grep -vE '#[[:space:]]*include[[:space:]]*"mogbus\.h"' || true)
	[ -n "$bad" ] || return 0
	printf 'Forbidden dependency include found (main.c may include only "mogbus.h"):\n' >&2
	printf '%s:%s\n' "$file" "$bad" >&2
	exit 1
}

check_tree() {
	root=$1
	check_layer "$root" net    "net modbus util os webui"
	check_layer "$root" webui  "webui util os"
	check_layer "$root" modbus "modbus device util os"
	check_layer "$root" device "device util os"
	check_layer "$root" config "config device server util os"
	check_layer "$root" server "server net modbus device util config os"
	check_layer "$root" client "client modbus net util os"
	check_layer "$root" view   "view device util os"
	check_layer "$root" api    "api client config device modbus net server view util os"
	check_layer "$root" util   "util os"
	check_layer "$root" os     "os"
	check_extra_rules "$root"
	check_os_headers "$root"
	check_main "$root"
}

self_test() {
	tmp=$(mktemp -d)
	trap 'rm -rf "$tmp"' EXIT HUP INT TERM

	clean=$tmp/clean
	mkdir -p "$clean/src/config" "$clean/src/device" "$clean/src/server" "$clean/src/api" \
		 "$clean/src/modbus" "$clean/src/net" "$clean/src/util" "$clean/src/os"
	printf '#include "server/server.h"\n' >"$clean/src/config/ok-server.c"
	printf '#include "config/config.h"\n#include "net/net_common.h"\n' >"$clean/src/server/server_config.c"
	printf '#include "net/net_common.h"\n' >"$clean/src/server/ok.c"
	printf '#include "server/server.h"\n#include "config/config.h"\n' >"$clean/src/api/ok.c"
	printf '#include "device/device.h"\n' >"$clean/src/config/ok.c"
	printf '#include "util/log.h"\n'       >"$clean/src/config/ok-util.c"
	printf '#include "util/log.h"\n'       >"$clean/src/device/ok.c"
	printf '#include "device/device.h"\n'  >"$clean/src/modbus/ok.c"
	printf '#include "util/log.h"\n'       >"$clean/src/modbus/ok-util.c"
	printf '#include "modbus/pdu.h"\n'     >"$clean/src/net/ok.c"
	printf '#include "util/log.h"\n'       >"$clean/src/net/ok-util.c"
	mkdir -p "$clean/src/webui"
	printf '#include "webui/webui.h"\n'    >"$clean/src/net/ok-webui.c"
	printf '#include "vlib/c_types.h"\n'   >"$clean/src/webui/ok.c"
	printf '#include "util/rng.h"\n'       >"$clean/src/util/ok.c"
	printf '#include "net/rtu_framer.h"\n' >"$clean/src/api/ok-net.c"
	mkdir -p "$clean/src/client"
	printf '#include "modbus/mbap.h"\n#include "os/os_wait.h"\n#include "util/log.h"\n' >"$clean/src/client/ok.c"
	printf '#include "net/net_tls.h"\n' >>"$clean/src/client/ok.c"
	printf '#include "client/client.h"\n' >>"$clean/src/api/ok-net.c"
	printf '#include "mogbus.h"\n#include <stdio.h>\n' >"$clean/src/main.c"
	printf '#include "os/os_socket.h"\n#include <poll.h>\n' >"$clean/src/os/ok.c" 2>/dev/null || true
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
	expect_fail client-net      client "net/net_common.h"
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

	expect_fail webui-net       webui  "net/net_obs.h"
	expect_fail webui-modbus    webui  "modbus/pdu.h"
	expect_fail webui-device    webui  "device/device.h"
	expect_fail webui-config    webui  "config/config.h"
	expect_fail webui-server    webui  "server/server.h"
	expect_fail webui-api       webui  "api/x.h"
	expect_fail util-webui      util   "webui/webui.h"
	expect_fail device-webui    device "webui/webui.h"
	expect_fail net-server      net    "server/server.h"
	expect_fail device-api      device "api/x.h"
	expect_fail config-server   config "server/server_config.h"
	expect_fail server-config   server "config/config.h"
	expect_fail client-net      client "net/net_common.h"
	expect_fail client-device   client "device/device.h"
	expect_fail client-server   client "server/server.h"
	expect_fail client-api      client "api/x.h"
	expect_fail net-client      net    "client/client.h"
	expect_fail server-client   server "client/client.h"

	for hdr in poll.h errno.h signal.h unistd.h sys/socket.h termios.h arpa/inet.h; do
		mkdir -p "$tmp/oshdr/src/device"
		printf '#include <%s>\n' "$hdr" >"$tmp/oshdr/src/device/bad.c"
		if ( check_tree "$tmp/oshdr" ) >/dev/null 2>&1; then
			printf 'check_deps.sh self-test: <%s> outside os/ did not fail\n' "$hdr" >&2
			exit 1
		fi
	done
	expect_fail os-util         os     "util/log.h"
	expect_fail view-net        view   "net/net_common.h"
	expect_fail view-server     view   "server/server.h"
	expect_fail view-api        view   "api/x.h"
	expect_fail device-view     device "view/view.h"
	expect_fail net-view        net    "view/view.h"

	mkdir -p "$tmp/main-internal/src"
	printf '#include "mogbus.h"\n#include "util/log.h"\n' >"$tmp/main-internal/src/main.c"
	if ( check_tree "$tmp/main-internal" ) >/dev/null 2>&1; then
		printf 'check_deps.sh self-test: main-internal did not fail\n' >&2
		exit 1
	fi

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
