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

include_re='^#include[[:space:]]*[<"]([^">]+)[">]'

check_forbidden \
	'config/device must not include modbus/net' \
	"$include_re"'.*(((\.\./)*)?(modbus|net)/)' \
	src/config src/device

check_forbidden \
	'modbus must not include net/config' \
	"$include_re"'.*(((\.\./)*)?(net|config)/)' \
	src/modbus

check_forbidden \
	'util must not include net/modbus/device/config' \
	"$include_re"'.*(((\.\./)*)?(net|modbus|device|config)/)' \
	src/util
