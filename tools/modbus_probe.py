#!/usr/bin/env python3
"""modbus_probe.py - tiny Modbus TCP client to try out mogbus. Python 3 standard library only.

Examples (server on localhost:1502):
  ./tools/modbus_probe.py holding 0 2            read 2 holding registers from address 0
  ./tools/modbus_probe.py input 0 2 --as float32 read input registers 0-1 as one float32
  ./tools/modbus_probe.py input 0 1 --watch 1    read every second until Ctrl-C
  ./tools/modbus_probe.py coil 0 8               read 8 coils
  ./tools/modbus_probe.py write-reg 0 1234       write holding register 0
  ./tools/modbus_probe.py write-coil 0 on        switch coil 0 on
  ./tools/modbus_probe.py --unit 2 input 0 2     talk to the device with unit id 2
"""
import argparse
import socket
import struct
import sys
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


def decode(words, kind, low_first):
    """Groups registers into 32-bit values when --as asks for it."""
    if kind == "u16":
        return words
    if kind == "s16":
        return [w - 65536 if w >= 32768 else w for w in words]
    pairs = [words[i:i + 2] for i in range(0, len(words) - 1, 2)]
    raw = [(p[1] << 16 | p[0]) if low_first else (p[0] << 16 | p[1]) for p in pairs]
    fmt = {"u32": ">I", "s32": ">i", "float32": ">f"}[kind]
    out = []
    for r in raw:
        value = struct.unpack(fmt, struct.pack(">I", r))[0]
        out.append(value)
    return out


def run(args, sock):
    unit = args.unit
    if args.command in ("coil", "discrete"):
        function = 1 if args.command == "coil" else 2
        print(read_bits(sock, unit, function, args.address, args.count))
    elif args.command in ("holding", "input"):
        function = 3 if args.command == "holding" else 4
        words = read_registers(sock, unit, function, args.address, args.count)
        print(decode(words, args.as_type, args.low_first))
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
    parser.add_argument("--timeout", type=float, default=3.0, help="seconds to wait for a response")
    parser.add_argument("--watch", type=float, metavar="SECONDS", help="repeat every SECONDS")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("coil", "discrete", "holding", "input"):
        p = sub.add_parser(name)
        p.add_argument("address", type=int)
        p.add_argument("count", type=int, nargs="?", default=1)
        if name in ("holding", "input"):
            p.add_argument("--as", dest="as_type", default="u16",
                           choices=["u16", "s16", "u32", "s32", "float32"])
            p.add_argument("--low-first", action="store_true", help="low word is the first register")
    p = sub.add_parser("write-reg")
    p.add_argument("address", type=int)
    p.add_argument("value", type=int)
    p = sub.add_parser("write-coil")
    p.add_argument("address", type=int)
    p.add_argument("value", help="on or off")
    args = parser.parse_args()

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
