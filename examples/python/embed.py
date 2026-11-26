#!/usr/bin/env python3
"""Python counterpart of examples/embed_tcp.c.

Builds a device in code, serves it as Modbus TCP on a free port from a background thread, reads
it with a tiny raw MBAP client, changes a value from Python while the server runs, and reads again.
Exit status 0 means every value read over TCP matched.

From a checkout:  make python && PYTHONPATH=python python3 examples/python/embed.py
"""
import socket
import struct
import sys
from pathlib import Path

import mogbus

failures = 0


def request(port, tid, fc, address, count):
    """Sends one read request (FC 01-04) and returns the data bytes of the reply."""
    # MBAP: transaction id, protocol 0, length 6, unit 1; then FC, address, count.
    frame = struct.pack(">HHHBBHH", tid, 0, 6, 1, fc, address, count)
    with socket.create_connection(("127.0.0.1", port), timeout=2) as s:
        s.sendall(frame)
        reply = b""
        while len(reply) < 9 or len(reply) < 6 + struct.unpack(">H", reply[4:6])[0]:
            chunk = s.recv(256)
            if not chunk:
                break
            reply += chunk
    if len(reply) < 9 or reply[7] != fc:
        raise RuntimeError(f"unexpected reply {reply.hex()}")
    return reply[9:9 + reply[8]]


def check(what, got, want):
    global failures
    ok = got == want
    failures += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {what}: {got!r}" + ("" if ok else f" (wanted {want!r})"))


print("mogbus", mogbus.library_version())

# Part 1: a device built in code.
with mogbus.Context() as ctx:
    dev = ctx.add_device(1, "embedded")
    dev.add_point("holding", 10, "u16", 1234)
    dev.add_point("input", 0, "f32", 21.5)
    dev.add_point("coil", 3, "bool", 1)

    # Ask the kernel for a free loopback port; start() serves from a background thread.
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    server = ctx.serve(port=port).start()

    check("holding 10", struct.unpack(">H", request(port, 1, 3, 10, 1))[0], 1234)
    check("input 0 (float32)", struct.unpack(">f", request(port, 2, 4, 0, 2))[0], 21.5)
    check("coil 3", request(port, 3, 1, 3, 1)[0] & 1, 1)

    # The host changes values while the server runs; the next request sees them.
    dev.set_value("holding", 10, 4321)
    dev.set_value("input", 0, 22.25)
    check("holding 10 after set_value", struct.unpack(">H", request(port, 4, 3, 10, 1))[0], 4321)
    check("input 0 after set_value", struct.unpack(">f", request(port, 5, 4, 0, 2))[0], 22.25)

    for line in server.metrics().splitlines():
        if line.startswith("mogbus_requests_total{"):
            print(line)
    server.close()

# Part 2: the same thing from a profile, on a free port.
profile = Path(__file__).resolve().parents[2] / "profiles" / "minimal.scm"
with mogbus.serve(config=str(profile), port=0) as srv:
    check("minimal.scm holding 0", struct.unpack(">H", request(srv.port, 1, 3, 0, 1))[0], 42)

sys.exit(1 if failures else 0)
