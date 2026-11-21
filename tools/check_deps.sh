#!/bin/sh
set -eu

# Enforce documented dependency direction: config/device must not depend on modbus/net.
bad=$(grep -rnE '#include[[:space:]]*[<"]((modbus|net)/|(\.\./)*(modbus|net)/)' src/config src/device || true)
if [ -n "$bad" ]; then
	echo "Forbidden dependency includes found:" >&2
	echo "$bad" >&2
	exit 1
fi
