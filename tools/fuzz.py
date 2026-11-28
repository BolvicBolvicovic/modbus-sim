#!/usr/bin/env python3
"""Runs the libFuzzer harnesses built by `make fuzz`. Usage: tools/fuzz.py SECONDS NAME...

Seeds build/fuzz/corpus/<name> with one valid request per supported function code, framed for each
harness, then runs every harness for SECONDS, at most FUZZ_JOBS (default nproc) at once, continuing
from the corpus a previous run left there. A crash fails the run (exit status 1); libFuzzer writes
the reproducer to build/fuzz/artifacts/<name>-crash-*, and `build/fuzz/fuzz_<name> FILE` replays it.
Results print in argument order."""
import concurrent.futures
import os
import re
import struct
import subprocess
import sys

DIR = 'build/fuzz'

PDUS = [
    bytes([0x01, 0, 0, 0, 8]),                 # read coils
    bytes([0x02, 0, 0, 0, 8]),                 # read discrete inputs
    bytes([0x03, 0, 0, 0, 4]),                 # read holding registers
    bytes([0x04, 0, 0, 0, 2]),                 # read input registers
    bytes([0x05, 0, 1, 0xFF, 0]),              # write single coil
    bytes([0x06, 0, 2, 0x12, 0x34]),           # write single register
    bytes([0x07]),                             # read exception status
    bytes([0x08, 0, 0, 0xB0, 0x6B]),           # diagnostics: return query data
    bytes([0x08, 0, 0x0A, 0, 0]),              # diagnostics: clear counters
    bytes([0x0B]),                             # comm event counter
    bytes([0x0C]),                             # comm event log
    bytes([0x0F, 0, 0, 0, 10, 2, 0xFF, 0x03]), # write multiple coils
    bytes([0x10, 0, 0, 0, 2, 4, 0, 1, 0, 2]),  # write multiple registers
    bytes([0x11]),                             # report server id
    bytes([0x14, 7, 6, 0, 1, 0, 0, 0, 2]),     # read file record
    bytes([0x15, 9, 6, 0, 1, 0, 0, 0, 1, 0xAB, 0xCD]),  # write file record
    bytes([0x16, 0, 3, 0xFF, 0x00, 0x00, 0x0F]),         # mask write register
    bytes([0x17, 0, 0, 0, 2, 0, 4, 0, 1, 2, 0, 9]),      # read/write multiple registers
    bytes([0x18, 0, 5]),                       # read FIFO queue
    bytes([0x2B, 0x0E, 1, 0]),                 # read device identification
    bytes([0x2B, 0x0D, 0x00, 0x01, 0x60, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00]),  # CANopen SDO upload
    bytes([0x41, 0x01]), bytes([0x41, 0x02]), bytes([0x41, 0x03]),             # user-defined 0x41
    bytes([0x41, 0x04, 0x04, 0xAA]), bytes([0x41, 0x05]), bytes([0x41, 0x09]),
]


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return struct.pack('<H', crc)


def rtu(unit, pdu):
    frame = bytes([unit]) + pdu
    return frame + crc16(frame)


def ascii_frame(unit, pdu):
    body = bytes([unit]) + pdu
    lrc = (-sum(body)) & 0xFF
    return b':' + (body + bytes([lrc])).hex().upper().encode() + b'\r\n'


def mbap(tid, unit, pdu):
    return struct.pack('>HHHB', tid, 0, len(pdu) + 1, unit) + pdu


def http_seeds():
    """First byte: transport rules (bit 0 TCP-like, bit 1 MBAP), then one HTTP request."""
    requests = [
        'GET /healthz HTTP/1.1\r\n\r\n',
        'GET /metrics HTTP/1.1\r\n\r\n',
        'GET /api/devices HTTP/1.1\r\n\r\n',
        'GET /api/devices/1/holding/0?count=4 HTTP/1.1\r\n\r\n',
        'GET /api/devices/1/coil/0?count=24 HTTP/1.1\r\n\r\n',
        'GET /api/devices/1/holding/100/value HTTP/1.1\r\n\r\n',
        'GET /api/devices/1/faults HTTP/1.1\r\n\r\n',
        'GET /api/devices/1/points?space=holding HTTP/1.1\r\n\r\n',
        'GET /api/devices/1/points?space=input&from=5&limit=2 HTTP/1.1\r\n\r\n',
        'GET /api/devices/1/actions HTTP/1.1\r\n\r\n',
        'GET /api/devices/1/actions?from=1&limit=1 HTTP/1.1\r\n\r\n',
        'DELETE /api/devices/1/faults/delay HTTP/1.1\r\n\r\n',
    ]
    bodies = [
        ('PUT /api/devices/1/holding/0', '{"values":[1,2,3]}'),
        ('PUT /api/devices/1/coil/0', '{"values":[1,0,1]}'),
        ('PUT /api/devices/1/holding/100/value', '{"value":-12.5}'),
        ('PUT /api/devices/1/actions/holding/0/read', '{"name":"step","value":5}'),
        ('PUT /api/devices/1/actions/holding/0/read', '{"name":"on","value":0}'),
        ('PUT /api/devices/1/faults/delay', '{"delay_ms":100,"percent":50}'),
        ('PUT /api/devices/1/faults/exception', '{"exception_code":6}'),
        ('PUT /api/devices/1/faults/corrupt', '{"mode":"checksum"}'),
        ('PUT /api/devices/1/faults/disconnect', '{}'),
        ('PUT /api/devices/1/faults/drop', '{"percent":10}'),
    ]
    out = [('\x03' + r).encode() for r in requests]
    for path, body in bodies:
        req = f'{path} HTTP/1.1\r\nContent-Length: {len(body)}\r\n\r\n{body}'
        out += [('\x03' + req).encode(), ('\x00' + req).encode()]
    return out


def write_seeds(out):
    seeds = {'pdu': [], 'mbap': [], 'rtu': [], 'ascii': [], 'decode': [], 'http': http_seeds()}
    for i, pdu in enumerate(PDUS):
        seeds['pdu'].append(pdu)
        seeds['mbap'].append(mbap(i, 1, pdu))
        seeds['mbap'].append(mbap(i, 9, pdu))
        frame = rtu(1, pdu)
        # flags byte (baud known), then the frame in one chunk followed by a gap
        seeds['rtu'].append(bytes([1, 0x80 | (len(frame) - 1 & 0x1F)]) + frame[:32])
        seeds['ascii'].append(ascii_frame(1, pdu) + ascii_frame(0, pdu))
        # framing, direction, json bit and small-buffer size, then the frame (auto and forced)
        for n, framed in enumerate((mbap(i, 1, pdu), frame, ascii_frame(1, pdu))):
            seeds['decode'].append(bytes([0, 0, i & 1]) + framed)
            seeds['decode'].append(bytes([n + 1, 1 + i % 2, 0x60 | (i & 1)]) + framed)
    for name, items in seeds.items():
        os.makedirs(os.path.join(out, name), exist_ok=True)
        for i, data in enumerate(items):
            with open(os.path.join(out, name, f'seed-{i:02d}'), 'wb') as f:
                f.write(data)


def run_one(name, seconds):
    with open(f'{DIR}/{name}.log', 'w') as log:
        rc = subprocess.call([f'{DIR}/fuzz_{name}', f'-max_total_time={seconds}', '-print_final_stats=1',
                              f'-artifact_prefix={DIR}/artifacts/{name}-', f'{DIR}/corpus/{name}'],
                             stdout=log, stderr=subprocess.STDOUT)
    return rc == 0


def main():
    seconds, names = sys.argv[1], sys.argv[2:]
    jobs = int(os.environ.get('FUZZ_JOBS') or os.cpu_count() or 1)
    os.makedirs(f'{DIR}/artifacts', exist_ok=True)
    write_seeds(f'{DIR}/corpus')
    print(f'fuzzing {len(names)} harnesses, {seconds}s each, up to {jobs} at once', flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(lambda n: run_one(n, seconds), names))
    fail = False
    for name, ok in zip(names, results):
        print(f'fuzz_{name}: {seconds}s')
        with open(f'{DIR}/{name}.log', errors='replace') as f:
            lines = f.read().splitlines()
        if not ok:
            print('\n'.join(lines[-40:]), file=sys.stderr)
            print(f'fuzz_{name}: FAILED (see {DIR}/{name}.log and {DIR}/artifacts)', file=sys.stderr)
            fail = True
            continue
        print(' '.join(l for l in lines if re.match(r'stat::(number_of_executed_units|peak_rss_mb)', l)) + ' ')
    return 1 if fail else 0


if __name__ == '__main__':
    sys.exit(main())
