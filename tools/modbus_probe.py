#!/usr/bin/env python3
"""modbus_probe.py - tiny Modbus TCP client to try out mogbus. Python 3 standard library only.

Examples (server on localhost:1502):
  ./tools/modbus_probe.py holding 0 2            read 2 holding registers from address 0
  ./tools/modbus_probe.py input 0 2 --as float32 read input registers 0-1 as one float32
  ./tools/modbus_probe.py holding 100 4 --as u64 read 4 registers as one u64 (also s64, float64)
  ./tools/modbus_probe.py input 8 2 --as float32 --byte-order little   byte-swapped float32 (BADC)
  ./tools/modbus_probe.py input 20 8 --as string read 8 registers as text
  ./tools/modbus_probe.py input 0 1 --watch 1    read every second until Ctrl-C
  ./tools/modbus_probe.py coil 0 8               read 8 coils
  ./tools/modbus_probe.py write-reg 0 1234       write holding register 0
  ./tools/modbus_probe.py write-coil 0 on        switch coil 0 on
  ./tools/modbus_probe.py --unit 2 input 0 2     talk to the device with unit id 2

Load and stability test: `load` runs --clients threads, each with its own TCP connection, sending
FC 04/03 reads back to back to the given unit ids (round-robin) for --duration seconds. It prints
throughput, latency percentiles, errors and (with --pid or --spawn) the server's resident memory at
start, peak and end, and exits non-zero if any request failed or memory grew more than
--max-rss-growth-kb.
  ./tools/modbus_probe.py load --spawn profiles/two-devices.scm --units 1,2 --clients 32 --duration 60
  ./tools/modbus_probe.py load --port 1502 --units 1 --clients 8 --duration 3600 --pid "$(pidof mogbus)"
"""
import argparse
import os
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

EXCEPTIONS = {1: "illegal function", 2: "illegal data address", 3: "illegal data value",
              4: "server device failure", 5: "acknowledge", 6: "server device busy",
              7: "negative acknowledge", 8: "memory parity error", 10: "gateway path unavailable",
              11: "gateway target failed to respond"}
_transaction = 0


def request(sock, unit, pdu):
    """Sends one request PDU and returns the response PDU. Raises on exception responses."""
    global _transaction
    _transaction = (_transaction + 1) & 0xFFFF
    sock.sendall(struct.pack(">HHHB", _transaction, 0, len(pdu) + 1, unit) + pdu)
    header = recv_exact(sock, 7)
    tid, proto, length, _unit = struct.unpack(">HHHB", header)
    body = recv_exact(sock, length - 1)
    if body[0] & 0x80:
        raise RuntimeError("exception 0x%02X: %s" % (body[1], EXCEPTIONS.get(body[1], "?")))
    return body


def recv_exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("connection closed")
        data += chunk
    return data


def read_registers(sock, unit, function, address, qty):
    body = request(sock, unit, struct.pack(">BHH", function, address, qty))
    return list(struct.unpack(">%dH" % qty, body[2:2 + qty * 2]))


def read_bits(sock, unit, function, address, qty):
    body = request(sock, unit, struct.pack(">BHH", function, address, qty))
    return [(body[2 + i // 8] >> (i % 8)) & 1 for i in range(qty)]


def decode(words, kind, low_first, little=False):
    """Groups registers into 16, 32 or 64-bit values (or one string) when --as asks for it.

    low_first: the least significant register comes first. little: the two bytes inside every
    register are swapped (--byte-order little).
    """
    if little:
        words = [((w & 0xFF) << 8) | (w >> 8) for w in words]
    if kind == "string":
        text = b"".join(struct.pack(">H", w) for w in words).split(b"\0")[0]
        return [text.decode("latin-1")]
    if kind == "u16":
        return words
    if kind == "s16":
        return [w - 65536 if w >= 32768 else w for w in words]
    width = 4 if kind in ("u64", "s64", "float64") else 2
    fmt = {"u32": ">I", "s32": ">i", "float32": ">f", "u64": ">Q", "s64": ">q", "float64": ">d"}[kind]
    out = []
    for i in range(0, len(words) - width + 1, width):
        group = words[i:i + width][::-1] if low_first else words[i:i + width]
        out.append(struct.unpack(fmt, struct.pack(">%dH" % width, *group))[0])
    return out


LOAD_FC = {"input": 0x04, "holding": 0x03}


def rss_kb(pid):
    try:
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        return None
    return None


def load_client(args, index, stop, results):
    latencies = []
    errors = 0
    tid = index << 8
    units = args.units
    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except OSError as e:
        results[index] = ([], 1, "connect: %s" % e)
        return
    pdu = struct.pack(">BHH", LOAD_FC[args.table], args.address, args.count)
    n = 0
    last_error = None
    while not stop.is_set():
        tid = (tid + 1) & 0xFFFF
        unit = units[n % len(units)]
        n += 1
        start = time.perf_counter()
        try:
            sock.sendall(struct.pack(">HHHB", tid, 0, len(pdu) + 1, unit) + pdu)
            r_tid, proto, length, r_unit = struct.unpack(">HHHB", recv_exact(sock, 7))
            body = recv_exact(sock, length - 1)
            if r_tid != tid or proto != 0 or r_unit != unit or body[0] != pdu[0]:
                raise ValueError("unexpected reply %s" % body[:2].hex())
        except (OSError, ValueError, ConnectionError) as e:
            errors += 1
            last_error = str(e)
            break   # a broken connection cannot continue
        latencies.append(time.perf_counter() - start)
    sock.close()
    results[index] = (latencies, errors, last_error)


def wait_port(host, port, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            socket.create_connection((host, port), timeout=0.5).close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


def percentile(sorted_values, p):
    if not sorted_values:
        return 0.0
    k = min(len(sorted_values) - 1, int(round(p / 100.0 * (len(sorted_values) - 1))))
    return sorted_values[k]


def load(args):
    """The `load` subcommand: returns the exit status."""

    proc = None
    if args.spawn:
        proc = subprocess.Popen([os.environ.get("MOGBUS_BIN", "./mogbus"), args.spawn],
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        args.pid = proc.pid
        if not wait_port(args.host, args.port, 5):
            proc.kill()
            print("server did not start: %s" % proc.stderr.read().decode(errors="replace"), file=sys.stderr)
            return 1

    rss_start = rss_kb(args.pid) if args.pid else None
    rss_peak = rss_start
    stop = threading.Event()
    results = [None] * args.clients
    threads = [threading.Thread(target=load_client, args=(args, i, stop, results)) for i in range(args.clients)]
    began = time.perf_counter()
    for t in threads:
        t.start()
    while time.perf_counter() - began < args.duration:
        time.sleep(min(1.0, args.duration))
        if args.pid:
            now = rss_kb(args.pid)
            if now is not None and (rss_peak is None or now > rss_peak):
                rss_peak = now
    stop.set()
    for t in threads:
        t.join()
    elapsed = time.perf_counter() - began
    rss_end = rss_kb(args.pid) if args.pid else None

    exit_code = 0
    if proc:
        proc.send_signal(signal.SIGTERM)
        try:
            exit_code = proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            exit_code = -1

    latencies = sorted(l for r in results if r for l in r[0])
    errors = sum(r[1] for r in results if r)
    messages = sorted({r[2] for r in results if r and r[2]})
    print("clients %d, units %s, %.1f s" % (args.clients, ",".join(map(str, args.units)), elapsed))
    print("requests %d, %.0f req/s, errors %d" % (len(latencies), len(latencies) / elapsed, errors))
    print("latency ms: p50 %.3f  p95 %.3f  p99 %.3f  max %.3f" % tuple(
        1000 * v for v in (percentile(latencies, 50), percentile(latencies, 95),
                           percentile(latencies, 99), latencies[-1] if latencies else 0)))
    for m in messages:
        print("error: %s" % m)
    failed = errors > 0
    if rss_start is not None and rss_end is not None:
        print("server RSS kB: start %d  peak %d  end %d" % (rss_start, rss_peak, rss_end))
        if rss_end - rss_start > args.max_rss_growth_kb:
            print("FAIL: resident memory grew by %d kB" % (rss_end - rss_start))
            failed = True
    if proc:
        print("server exit status %d" % exit_code)
        failed = failed or exit_code != 0
    return 1 if failed else 0



def run(args, sock):
    unit = args.unit
    if args.command in ("coil", "discrete"):
        function = 1 if args.command == "coil" else 2
        print(read_bits(sock, unit, function, args.address, args.count))
    elif args.command in ("holding", "input"):
        function = 3 if args.command == "holding" else 4
        words = read_registers(sock, unit, function, args.address, args.count)
        print(decode(words, args.as_type, args.low_first, args.byte_order == "little"))
    elif args.command == "write-reg":
        request(sock, unit, struct.pack(">BHH", 6, args.address, args.value & 0xFFFF))
        print("ok")
    elif args.command == "write-coil":
        on = args.value.lower() in ("on", "1", "true", "yes")
        request(sock, unit, struct.pack(">BHH", 5, args.address, 0xFF00 if on else 0))
        print("ok")


def main():
    parser = argparse.ArgumentParser(description="Tiny Modbus TCP client for mogbus")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=1502)
    parser.add_argument("--unit", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=None,
                        help="seconds to wait for a response (default 3, 2 for load)")
    parser.add_argument("--watch", type=float, metavar="SECONDS", help="repeat every SECONDS")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("coil", "discrete", "holding", "input"):
        p = sub.add_parser(name)
        p.add_argument("address", type=int)
        p.add_argument("count", type=int, nargs="?", default=1)
        if name in ("holding", "input"):
            p.add_argument("--as", dest="as_type", default="u16",
                           choices=["u16", "s16", "u32", "s32", "float32", "u64", "s64", "float64",
                                    "string"])
            p.add_argument("--low-first", action="store_true",
                           help="least significant register first (32/64-bit types)")
            p.add_argument("--byte-order", choices=["big", "little"], default="big",
                           help="little: the two bytes inside every register are swapped")
    p = sub.add_parser("write-reg")
    p.add_argument("address", type=int)
    p.add_argument("value", type=int)
    p = sub.add_parser("write-coil")
    p.add_argument("address", type=int)
    p.add_argument("value", help="on or off")
    p = sub.add_parser("load", help="multi-client load and stability test")
    for opt, kw in (("--host", {}), ("--port", {"type": int}), ("--timeout", {"type": float})):
        p.add_argument(opt, default=argparse.SUPPRESS, help=argparse.SUPPRESS, **kw)
    p.add_argument("--units", default="1", help="comma-separated unit ids, used round-robin")
    p.add_argument("--table", choices=sorted(LOAD_FC), default="input")
    p.add_argument("--address", type=int, default=0)
    p.add_argument("--count", type=int, default=1)
    p.add_argument("--clients", type=int, default=8, help="concurrent connections (server max 32)")
    p.add_argument("--duration", type=float, default=10, help="seconds")
    p.add_argument("--pid", type=int, help="server pid, for memory sampling")
    p.add_argument("--spawn", metavar="CONFIG", help="start mogbus (MOGBUS_BIN or ./mogbus) with CONFIG")
    p.add_argument("--max-rss-growth-kb", type=int, default=1024,
                   help="fail if resident memory grows more than this (default 1024)")
    args = parser.parse_args()
    if args.timeout is None:
        args.timeout = 2.0 if args.command == "load" else 3.0
    if args.command == "load":
        args.units = [int(u) for u in args.units.split(",")]
        return load(args)

    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
        while True:
            run(args, sock)
            if not args.watch:
                break
            time.sleep(args.watch)
    except socket.timeout:
        print("timeout: no response", file=sys.stderr)
        return 3
    except (RuntimeError, ConnectionError, OSError) as err:
        print("error: %s" % err, file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
