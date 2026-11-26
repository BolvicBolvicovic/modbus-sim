#!/usr/bin/env python3
"""pymodbus_interop.py - checks mogbus against an independent Modbus client stack (pymodbus).

pymodbus is a widely used open-source Modbus implementation (it is inside many Python SCADA
gateways and test tools). For each network transport this script starts mogbus with
examples/interop/device.scm, runs every supported function code through pymodbus' own framing
and decoding, and compares the answers with the register map of device.scm. It prints a
check-by-mode table and exits non-zero on any mismatch.

  python3 -m venv .venv && .venv/bin/pip install 'pymodbus==3.8.6'
  .venv/bin/python examples/interop/pymodbus_interop.py            # all modes
  .venv/bin/python examples/interop/pymodbus_interop.py tcp rtu-tcp

MOGBUS_BIN selects the binary (default ./mogbus). See examples/interop/README.md.
"""
import os
import socket
import subprocess
import sys
import tempfile
import time

from pymodbus.client import ModbusTcpClient, ModbusUdpClient
from pymodbus.framer import FramerType
from pymodbus.pdu import ModbusPDU
from pymodbus.pdu.file_message import FileRecord

HERE = os.path.dirname(os.path.abspath(__file__))
MODES = {
    # mode: (client class, framer)
    "tcp": (ModbusTcpClient, FramerType.SOCKET),
    "udp": (ModbusUdpClient, FramerType.SOCKET),
    "rtu-tcp": (ModbusTcpClient, FramerType.RTU),
    "rtu-udp": (ModbusUdpClient, FramerType.RTU),
    "ascii-tcp": (ModbusTcpClient, FramerType.ASCII),
    "ascii-udp": (ModbusUdpClient, FramerType.ASCII),
}


class VendorRequest(ModbusPDU):
    """User-defined function code 0x41: one selector byte after the function code."""

    function_code = 0x41
    rtu_frame_size = 5

    def __init__(self, selector=0, dev_id=1, transaction_id=0):
        super().__init__(dev_id=dev_id, transaction_id=transaction_id)
        self.selector = selector

    def encode(self):
        return bytes([self.selector])

    def decode(self, data):
        self.selector = data[0]


class VendorResponse(ModbusPDU):
    """Answer of 0x41: a byte count and that many data bytes (what the device.scm cases send)."""

    function_code = 0x41
    rtu_byte_count_pos = 2

    def encode(self):
        return bytes([len(self.data)]) + self.data

    def decode(self, data):
        self.data = bytes(data[1:1 + data[0]])


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def ok(result):
    return result is not None and not result.isError()


# Failures that are bugs of the client stack, not of mogbus: reported, not counted.
KNOWN_CLIENT_ISSUES = {
    # pymodbus 3.8.6 ReadFifoQueueResponse.decode() reads `count - 4` values (and encode()
    # writes the byte length as the FIFO count), so it decodes a spec-correct 3-value FIFO as
    # empty. mogbus answers 18 00 08 00 03 00 01 00 02 00 03 (byte count 2 + 2*N, FIFO count N).
    "fifo": "pymodbus 3.8.6 FC 24 decoder bug",
}


def swapped(registers):
    """Swaps the two bytes of every register. pymodbus has no byte-order option (word_order only
    reorders registers), so a byte-swapped layout is decoded by undoing the swap first."""
    return [((r & 0xFF) << 8) | (r >> 8) for r in registers]


def vendor(c, selector):
    """Sends user-defined function code 0x41 with one selector byte; returns the response PDU."""
    return c.execute(False, VendorRequest(selector))


def checks(c):
    """(name, function code, callable returning True when the answer is right[, known issue])."""
    float32 = c.DATATYPE.FLOAT32
    dt = c.DATATYPE

    def holding(address, count):
        r = c.read_holding_registers(address, count=count)
        return r.registers if ok(r) else None

    return [
        ("read coils", 0x01, lambda: ok(r := c.read_coils(0, count=16)) and not any(r.bits[:16])),
        ("read discrete inputs", 0x02, lambda: ok(r := c.read_discrete_inputs(0, count=8))
         and r.bits[:8] == [True] + [False] * 7),
        ("read holding registers", 0x03, lambda: ok(r := c.read_holding_registers(0, count=2))
         and r.registers[0] == 1234),
        ("read input registers (float32)", 0x04, lambda: ok(r := c.read_input_registers(0, count=2))
         and abs(c.convert_from_registers(r.registers, float32) - 230.5) < 1e-3),
        ("write single coil", 0x05, lambda: ok(c.write_coil(3, True))
         and c.read_coils(3, count=1).bits[0] is True),
        ("write single register", 0x06, lambda: ok(c.write_register(5, 42))
         and c.read_holding_registers(5, count=1).registers == [42]),
        ("read exception status", 0x07, lambda: ok(c.read_exception_status())),
        ("diagnostics: return query data", 0x08, lambda: ok(r := c.diag_query_data(b"\xb0\x6b"))
         and r.message in (b"\xb0\x6b", [0xB06B])),
        ("diagnostics: bus message count", 0x08, lambda: ok(r := c.diag_read_bus_message_count())
         and r.message > 0),
        ("get comm event counter", 0x0B, lambda: ok(c.diag_get_comm_event_counter())),
        ("get comm event log", 0x0C, lambda: ok(c.diag_get_comm_event_log())),
        ("write multiple coils", 0x0F, lambda: ok(c.write_coils(8, [True, False, True]))
         and c.read_coils(8, count=3).bits[:3] == [True, False, True]),
        ("write multiple registers", 0x10, lambda: ok(c.write_registers(6, [7, 8, 9]))
         and c.read_holding_registers(6, count=3).registers == [7, 8, 9]),
        ("report server id", 0x11, lambda: ok(c.report_slave_id())),
        # pymodbus' FileRecord takes record_length in bytes: 6 bytes = 3 registers.
        ("read file record", 0x14, lambda: ok(r := c.read_file_record([FileRecord(file_number=1,
                                                                                 record_number=0,
                                                                                 record_length=6)]))
         and r.records[0].record_data == bytes.fromhex("123456789abc")),
        ("mask write register", 0x16, lambda: ok(c.write_register(9, 0x00F0))
         and ok(c.mask_write_register(address=9, and_mask=0x0F0F, or_mask=0x1000))
         and c.read_holding_registers(9, count=1).registers == [0x1000 | (0x00F0 & 0x0F0F)]),
        ("read/write multiple registers", 0x17, lambda: ok(r := c.readwrite_registers(
            read_address=0, read_count=1, write_address=2, values=[77])) and r.registers == [1234]
         and c.read_holding_registers(2, count=1).registers == [77]),
        ("read FIFO queue", 0x18, lambda: ok(r := c.read_fifo_queue(address=5)) and list(r.values) == [1, 2, 3],
         KNOWN_CLIENT_ISSUES.get("fifo")),
        ("read device identification", 0x2B, lambda: ok(r := c.read_device_information(read_code=1))
         and r.information.get(0) == b"mogbus" and r.information.get(1) == b"interop"),
        ("user-defined code: fixed reply", 0x41, lambda: ok(r := vendor(c, 0x01))
         and r.data == bytes.fromhex("deadbeef")),
        ("user-defined code: live registers", 0x41, lambda: ok(c.write_register(0, 1234))
         and ok(r := vendor(c, 0x02)) and r.data == bytes.fromhex("04d2") + bytes.fromhex("0000")),
        ("data type: u64", 0x03, lambda: (r := holding(100, 4)) is not None
         and c.convert_from_registers(r, dt.UINT64) == 0x0123456789ABCDEF),
        ("data type: s64", 0x03, lambda: (r := holding(104, 4)) is not None
         and c.convert_from_registers(r, dt.INT64) == -123456789012),
        ("data type: float64", 0x03, lambda: (r := holding(108, 4)) is not None
         and c.convert_from_registers(r, dt.FLOAT64) == -1234.5678),
        ("data type: u64, low-first", 0x03, lambda: (r := holding(112, 4)) is not None
         and c.convert_from_registers(r, dt.UINT64, word_order="little") == 0x0102030405060708),
        ("data type: float32, byte-order little", 0x03, lambda: (r := holding(116, 2)) is not None
         and c.convert_from_registers(swapped(r), float32) == 230.5),
        ("data type: float32, little + low-first", 0x03, lambda: (r := holding(118, 2)) is not None
         and c.convert_from_registers(swapped(r), float32, word_order="little") == 230.5),
        ("data type: s64, byte-order little", 0x03, lambda: (r := holding(120, 4)) is not None
         and c.convert_from_registers(swapped(r), dt.INT64) == -2),
        ("data type: string, NUL padded", 0x03, lambda: (r := holding(124, 8)) is not None
         and c.convert_from_registers(r, dt.STRING) == "mogbus-sim"),
        ("data type: string, byte-order little", 0x04, lambda: ok(r := c.read_input_registers(10, count=4))
         and c.convert_from_registers(swapped(r.registers), dt.STRING) == "ABCDEFGH"),
        ("data type: string, client write", 0x10, lambda: ok(c.write_registers(
            140, c.convert_to_registers("HELLO", dt.STRING)))
         and c.convert_from_registers(holding(140, 4), dt.STRING).startswith("HELLO")),
        ("exception 02 on undefined address", 0x03, lambda: (r := c.read_holding_registers(500, count=1))
         .isError() and getattr(r, "exception_code", None) == 2),
    ]


def run_mode(mode, results, notes):
    client_class, framer = MODES[mode]
    port = free_port()
    with open(os.path.join(HERE, "device.scm")) as f:
        device = f.read()
    with tempfile.NamedTemporaryFile("w", suffix=".scm", delete=False) as cfg:
        cfg.write("(server (mode %s) (port %d))\n%s" % (mode, port, device))
    proc = subprocess.Popen([os.environ.get("MOGBUS_BIN", "./mogbus"), cfg.name],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        time.sleep(0.3)
        if proc.poll() is not None:
            sys.exit("%s: mogbus did not start: %s" % (mode, proc.stderr.read().decode(errors="replace")))
        client = client_class("127.0.0.1", port=port, framer=framer, timeout=2, retries=0)
        if not client.connect():
            sys.exit("%s: pymodbus could not connect" % mode)
        client.register(VendorResponse)
        for name, fc, check, *known in checks(client):
            try:
                passed = bool(check())
            except Exception as e:     # a decoding error in either stack is a failure too
                passed = False
                name = "%s (%s)" % (name, type(e).__name__)
            if not passed and known and known[0]:
                passed = "known"
                notes.add("%s: %s" % (name, known[0]))
            results.setdefault((fc, name), {})[mode] = passed
        client.close()
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        os.unlink(cfg.name)


def main():
    modes = sys.argv[1:] or list(MODES)
    unknown = [m for m in modes if m not in MODES]
    if unknown:
        sys.exit("unknown mode(s): %s (choose from %s)" % (", ".join(unknown), ", ".join(MODES)))
    results = {}
    notes = set()
    for mode in modes:
        run_mode(mode, results, notes)
    width = max(len(name) for _, name in results) + 8
    print("%-*s %s" % (width, "check", " ".join("%-9s" % m for m in modes)))
    failures = 0
    for (fc, name), by_mode in sorted(results.items()):
        cells = []
        for m in modes:
            passed = by_mode.get(m)
            failures += passed is False
            cells.append("%-9s" % ("known" if passed == "known" else "pass" if passed else
                                   "FAIL" if passed is False else "-"))
        print("%-*s %s" % (width, "FC %02X %s" % (fc, name), " ".join(cells)))
    for note in sorted(notes):
        print("known client issue, not counted: %s" % note)
    print("%d check(s) failed" % failures if failures else "all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
