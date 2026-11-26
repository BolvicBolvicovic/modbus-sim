#!/usr/bin/env python3
"""loadtest.py - multi-client load and stability test for a mogbus Modbus TCP server.
Python 3 standard library only.

Each client is a thread with its own TCP connection that sends FC 04/03 reads back to back to
the given unit ids for the whole duration. At the end it prints throughput, latency percentiles,
errors and (with --pid or --spawn) the server's resident memory at start, peak and end, and exits
non-zero if any request failed or memory grew more than --max-rss-growth-kb.

Examples:
  ./tools/loadtest.py --spawn profiles/two-devices.scm --units 1,2 --clients 32 --duration 60
  ./tools/loadtest.py --port 1502 --units 1 --clients 8 --duration 3600 --pid "$(pidof mogbus)"
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

FC = {"input": 0x04, "holding": 0x03}


def recv_exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("server closed the connection")
        data += chunk
    return data


def rss_kb(pid):
    try:
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        return None
    return None


def client(args, index, stop, results):
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
    pdu = struct.pack(">BHH", FC[args.table], args.address, args.count)
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1502)
    ap.add_argument("--units", default="1", help="comma-separated unit ids, used round-robin")
    ap.add_argument("--table", choices=sorted(FC), default="input")
    ap.add_argument("--address", type=int, default=0)
    ap.add_argument("--count", type=int, default=1)
    ap.add_argument("--clients", type=int, default=8, help="concurrent connections (server max 32)")
    ap.add_argument("--duration", type=float, default=10, help="seconds")
    ap.add_argument("--timeout", type=float, default=2, help="per-request timeout, seconds")
    ap.add_argument("--pid", type=int, help="server pid, for memory sampling")
    ap.add_argument("--spawn", metavar="CONFIG", help="start mogbus (MOGBUS_BIN or ./mogbus) with CONFIG")
    ap.add_argument("--max-rss-growth-kb", type=int, default=1024,
                    help="fail if resident memory grows more than this (default 1024)")
    args = ap.parse_args()
    args.units = [int(u) for u in args.units.split(",")]

    proc = None
    if args.spawn:
        proc = subprocess.Popen([os.environ.get("MOGBUS_BIN", "./mogbus"), args.spawn],
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        args.pid = proc.pid
        if not wait_port(args.host, args.port, 5):
            proc.kill()
            sys.exit("server did not start: %s" % proc.stderr.read().decode(errors="replace"))

    rss_start = rss_kb(args.pid) if args.pid else None
    rss_peak = rss_start
    stop = threading.Event()
    results = [None] * args.clients
    threads = [threading.Thread(target=client, args=(args, i, stop, results)) for i in range(args.clients)]
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
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
