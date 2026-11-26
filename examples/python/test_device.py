"""Two tests against a served device. Needs pytest (see conftest.py)."""
import json
import socket
import struct
import urllib.request


def read_registers(port, fc, address, count):
    """Raw MBAP client; returns the reply PDU (FC, then data or an exception code)."""
    with socket.create_connection(("127.0.0.1", port), timeout=2) as s:
        s.sendall(struct.pack(">HHHBBHH", 1, 0, 6, 1, fc, address, count))
        reply = s.recv(256)
    return reply[7:]


def test_reads_configured_value(meter):
    pdu = read_registers(meter.port, 3, 0, 1)
    assert pdu == bytes([3, 2]) + struct.pack(">H", 42)


def test_set_value_and_fault(meter):
    meter.device(1).set_value("input", 0, 231.5)
    pdu = read_registers(meter.port, 4, 0, 2)
    assert struct.unpack(">f", pdu[2:])[0] == 231.5

    # A fault is injected over the HTTP control API; the client now gets exception 06 (busy).
    req = urllib.request.Request(
        f"http://127.0.0.1:{meter.http_port}/api/devices/1/faults/exception",
        data=json.dumps({"exception_code": 6}).encode(), method="PUT")
    assert urllib.request.urlopen(req, timeout=2).status == 200
    assert read_registers(meter.port, 3, 0, 1) == bytes([0x83, 6])
