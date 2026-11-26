#!/bin/sh
# Cross-compiles mogbus for Windows (MinGW-w64), runs the unit tests under Wine and checks
# mogbus.exe against Linux clients. CI and humans run the same script, on any host (or container)
# that has x86_64-w64-mingw32-gcc, make, wine, python3 and a native C compiler (the native
# mogbus is the --health client). Run from anywhere; it works in the repository root.
#
#   PYMODBUS=1 tools/win32_check.sh    also runs examples/interop/pymodbus_interop.py against
#                                      mogbus.exe (needs python3-venv and network access for pip)
#
# Variables: WINE (default: wine, else wine64), MAKE_JOBS (default nproc), WIN_MAKE_ARGS (extra make arguments).
set -eu

cd "$(dirname "$0")/.."
root=$(pwd)
# Debian ships /usr/bin/wine, Ubuntu's wine64 only /usr/lib/wine/wine64 (plus a wine launcher).
if [ -z "${WINE:-}" ]; then
	for candidate in wine wine64 /usr/lib/wine/wine64; do
		command -v "$candidate" >/dev/null 2>&1 && { WINE=$candidate; break; }
	done
fi
WINE=${WINE:-wine}
jobs=${MAKE_JOBS:-$(nproc 2>/dev/null || echo 4)}
export WINEDEBUG=${WINEDEBUG:--all}
export WINEARCH=win64
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp}

for tool in x86_64-w64-mingw32-gcc make python3 "$WINE"; do
	command -v "$tool" >/dev/null 2>&1 || { echo "win32_check: $tool not found" >&2; exit 1; }
done

tmp=$(mktemp -d "${TMPDIR:-/tmp}/win32_check.XXXXXX")
export WINEPREFIX=$tmp/prefix
mogbus_pid=''
stop_server() {
	if [ -n "$mogbus_pid" ]; then
		kill -INT "$mogbus_pid" 2>/dev/null || true
		wait "$mogbus_pid" 2>/dev/null || true
		mogbus_pid=''
	fi
}
cleanup() {
	stop_server
	wineserver -k >/dev/null 2>&1 || true
	rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

step() { printf '\n== %s\n' "$*"; }
fail() { printf 'win32_check: FAIL: %s\n' "$*" >&2; exit 1; }

free_port() {
	python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'
}

# --- build -----------------------------------------------------------------------------------
step "build mogbus.exe, mogbus.dll and the test binary (PLATFORM=win32 WITH_TLS=0)"
# shellcheck disable=SC2086
make -j"$jobs" PLATFORM=win32 WITH_TLS=0 ${WIN_MAKE_ARGS:-} all lib mogbus-tests.exe
[ -f mogbus.exe ] && [ -f mogbus.dll ] && [ -f libmogbus.dll.a ] || fail "missing build output"

# The native binary is only a client here (--health); build it without TLS like the Windows one.
step "build the native mogbus (health client)"
make -j"$jobs" WITH_TLS=0 mogbus >/dev/null
native=$root/mogbus

step "prepare the Wine prefix"
"$WINE" wineboot --init >/dev/null 2>&1 || true

# --- unit tests --------------------------------------------------------------------------------
step "unit tests under Wine"
WINE=$WINE make PLATFORM=win32 WITH_TLS=0 tests

# --- integration -------------------------------------------------------------------------------
# Starts mogbus.exe on one mode and waits until the native --health sees it.
start_server() {
	mode=$1
	port=$2
	cfg=$tmp/$mode.scm
	cat >"$cfg" <<EOF
(server (mode $mode) (port $port) (http (port $3)))
(device (name "win32 check") (unit-id 1) (holding 0 (static 42)))
EOF
	"$WINE" ./mogbus.exe "$cfg" >"$tmp/$mode.log" 2>&1 &
	mogbus_pid=$!
	n=0
	until "$native" --health --timeout-ms 500 "$cfg" >/dev/null 2>&1; do
		n=$((n + 1))
		if [ "$n" -gt 40 ] || ! kill -0 "$mogbus_pid" 2>/dev/null; then
			cat "$tmp/$mode.log" >&2
			fail "$mode: mogbus.exe did not become healthy"
		fi
		sleep 0.25
	done
}

stop_checked() {
	pid=$mogbus_pid
	kill -INT "$pid"
	mogbus_pid=''
	wait "$pid" || { cat "$tmp/$1.log" >&2; fail "$1: mogbus.exe exited with status $? after SIGINT"; }
	grep -q 'stopped' "$tmp/$1.log" || { cat "$tmp/$1.log" >&2; fail "$1: no clean stop in the log"; }
}

for mode in tcp udp rtu-tcp rtu-udp ascii-tcp ascii-udp; do
	step "mogbus.exe in $mode mode: native --health, Windows --health, clean stop"
	port=$(free_port)
	http=$(free_port)
	start_server "$mode" "$port" "$http"
	"$native" --health "$tmp/$mode.scm" || fail "$mode: native --health"
	"$WINE" ./mogbus.exe --health "$tmp/$mode.scm" || fail "$mode: mogbus.exe --health"
	if [ "$mode" = tcp ]; then
		out=$(python3 tools/modbus_probe.py --port "$port" holding 0 1) || fail "tcp: modbus_probe"
		case "$out" in *42*) ;; *) fail "tcp: modbus_probe read '$out', expected 42" ;; esac
		python3 - "$http" <<'PY' || fail "tcp: HTTP endpoint"
import sys, urllib.request
base = "http://127.0.0.1:%s" % sys.argv[1]
assert urllib.request.urlopen(base + "/healthz").read() == b"ok\n"
assert b"mogbus_requests_total" in urllib.request.urlopen(base + "/metrics").read()
PY
	fi
	if [ "$mode" = udp ]; then
		# An oversized datagram is dropped (WSAEMSGSIZE on Windows) and the server keeps serving.
		python3 - "$port" <<'PY' || fail "udp: oversized datagram"
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(b"\x00" * 600, ("127.0.0.1", int(sys.argv[1])))
PY
		"$native" --health "$tmp/$mode.scm" || fail "udp: unhealthy after an oversized datagram"
	fi
	stop_checked "$mode"
done

# The same stop through SIGTERM (what a service manager or docker stop sends): Wine ends the process.
step "SIGTERM ends mogbus.exe"
port=$(free_port)
start_server tcp "$port" "$(free_port)"
kill -TERM "$mogbus_pid"
n=0
while kill -0 "$mogbus_pid" 2>/dev/null; do
	n=$((n + 1))
	[ "$n" -le 40 ] || fail "mogbus.exe ignored SIGTERM"
	sleep 0.25
done
wait "$mogbus_pid" 2>/dev/null || true
mogbus_pid=''

step "unsupported modes fail with a clear message"
printf '(server (mode rtu-pty))\n(device (name "p") (unit-id 1) (holding 0 (static 1)))\n' >"$tmp/pty.scm"
if "$WINE" ./mogbus.exe "$tmp/pty.scm" >"$tmp/pty.log" 2>&1; then fail "rtu-pty started on Windows"; fi
grep -q 'not available on Windows' "$tmp/pty.log" || { cat "$tmp/pty.log" >&2; fail "rtu-pty: unclear message"; }

# --- interop -----------------------------------------------------------------------------------
if [ -n "${PYMODBUS:-}" ]; then
	step "pymodbus interop against mogbus.exe"
	python3 -m venv "$tmp/venv"
	"$tmp/venv/bin/pip" install --quiet 'pymodbus==3.8.6'
	cat >"$tmp/mogbus-wine" <<EOF
#!/bin/sh
exec "$WINE" "$root/mogbus.exe" "\$@"
EOF
	chmod +x "$tmp/mogbus-wine"
	MOGBUS_START_DELAY=3 MOGBUS_BIN=$tmp/mogbus-wine "$tmp/venv/bin/python" examples/interop/pymodbus_interop.py \
		tcp udp rtu-tcp rtu-udp ascii-tcp ascii-udp
fi

step "win32_check: all checks passed"
