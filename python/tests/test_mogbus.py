import contextlib
import ctypes
import io
import json
import os
import re
import socket
import struct
import tempfile
import threading
import time
import unittest
from pathlib import Path

import mogbus
from mogbus import MogbusError, ServerOptions

CONFIG = """
(server (mode tcp) (port {port}))
(device (name "cfg") (unit-id 7)
  (holding 0 (static 42))
  (holding 1 (ramp (from 0) (to 100) (step 10) (every-ms 1000) (mode wrap))))
"""


DEVICES = CONFIG.split("\n", 2)[2]  # the config without its (server ...) line


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def mbap_read(sock, unit, address, count=1, fc=3):
    """Sends an MBAP read request and returns the register values (or the exception code)."""
    sock.sendall(struct.pack(">HHHBBHH", 1, 0, 6, unit, fc, address, count))
    reply = b""
    while len(reply) < 9:
        chunk = sock.recv(256)
        if not chunk:
            raise EOFError("connection closed")
        reply += chunk
    length = struct.unpack(">H", reply[4:6])[0]
    while len(reply) < 6 + length:
        reply += sock.recv(256)
    if reply[7] & 0x80:
        return ("exception", reply[8])
    return list(struct.unpack(">%dH" % (reply[8] // 2), reply[9:9 + reply[8]]))


class VersionTest(unittest.TestCase):
    def test_version(self):
        self.assertEqual(mogbus.library_version(), mogbus.__version__)

    def test_version_matches_header(self):
        header = Path(__file__).resolve().parents[2] / "include" / "mogbus.h"
        if not header.exists():
            self.skipTest("not in a checkout")
        text = header.read_text()
        parts = [next(l.split()[-1] for l in text.splitlines()
                      if l.startswith("#define MOGBUS_VERSION_" + n)) for n in
                 ("MAJOR", "MINOR", "PATCH")]
        self.assertEqual(".".join(parts), mogbus.__version__)

    def test_defaults_match_c(self):
        from mogbus import _lib
        c = _lib.lib.mogbus_server_options_defaults()
        self.assertEqual(ServerOptions._from_c(c), ServerOptions())


def header_fields(text, tag):
    """Field names of `struct TAG { ... };` in mogbus.h; nested anonymous structs
    give "outer.inner" names next to "outer"."""
    body = re.search(r"\nstruct %s\s*\{(.*?)\n\};" % tag, text, re.S).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    stack = [[]]
    for line in body.split("\n"):
        line = line.strip()
        if not line or line == "struct":
            continue
        if line == "{":
            stack.append([])
        elif line.startswith("}"):
            name = re.match(r"\}\s*(\w+)", line).group(1)
            inner = stack.pop()
            stack[-1].append(name)
            stack[-1].extend(f"{name}.{n}" for n in inner)
        else:
            for part in line.rstrip(";").split(","):
                part = re.sub(r"\[.*?\]", "", part).strip()
                stack[-1].append(part.split()[-1].lstrip("*"))
    assert len(stack) == 1
    return set(stack[0])


class LayoutTest(unittest.TestCase):
    """The ctypes structs must match the C structs field by field (see python/tests/layout.c)."""

    def test_layout(self):
        path = os.environ.get("MOGBUS_LAYOUT")
        if not path:
            self.skipTest("MOGBUS_LAYOUT not set; run `make python-tests`")
        from mogbus import _lib
        c = {}
        for line in Path(path).read_text().split("\n"):
            if line:
                name, *nums = line.split()
                c[name] = tuple(int(n) for n in nums)

        def mine(prefix, cls, nested):
            """Top-level fields plus the given (path, member, struct) nested ones."""
            out = {prefix: (ctypes.sizeof(cls),)}
            for name, _ in cls._fields_:
                d = getattr(cls, name)
                out[f"{prefix}.{name.rstrip('_')}"] = (d.offset, d.size)
            for base, inner in nested:
                off = getattr(cls, base.split("[")[0]).offset
                for name, _ in inner._fields_:
                    d = getattr(inner, name)
                    out[f"{prefix}.{base}.{name.rstrip('_')}"] = (off + d.offset, d.size)
            return out

        expected = {}
        expected.update(mine("O", _lib.ServerOptionsC, []))
        expected.update(mine("P", _lib.PointC, [("ramp", _lib._Ramp)]))
        expected.update(mine("I", _lib.DeviceInfoC, [("faults[0]", _lib._FaultInfo),
                                                     ("custom_fcs[0]", _lib._CustomFc)]))
        expected.update(mine("A", _lib.AccessC, []))
        expected.update(mine("FR", _lib.FrameC, []))
        expected.update(mine("CN", _lib.ConnectionC, []))
        expected.update(mine("CO", _lib.ClientOptionsC, []))
        expected.update(mine("PF", _lib.PollFdC, []))
        expected.update(mine("CF", _lib.ClientFileRecordC, []))
        for key in ("faults_count", "space_count"):
            self.assertIn(key, c)
        self.assertEqual(c.pop("faults_count"), (7,))
        self.assertEqual(c.pop("space_count"), (4,))
        self.assertEqual(c, expected)

        # The field lists come from the header itself, so a field added to a C struct without
        # a matching ctypes field or layout.c entry fails here.
        header = Path(__file__).resolve().parents[2] / "include" / "mogbus.h"
        if not header.exists():
            self.skipTest("include/mogbus.h not found")
        text = header.read_text()
        nested = {"P": {"ramp": _lib._Ramp},
                  "I": {"faults": _lib._FaultInfo, "custom_fcs": _lib._CustomFc}}
        for key, tag, cls in (("O", "mogbus_server_options_t", _lib.ServerOptionsC),
                              ("P", "mogbus_point_t", _lib.PointC),
                              ("I", "mogbus_device_info_t", _lib.DeviceInfoC),
                              ("A", "mogbus_access_t", _lib.AccessC),
                              ("FR", "mogbus_frame_t", _lib.FrameC),
                              ("CN", "mogbus_connection_t", _lib.ConnectionC),
                              ("CO", "mogbus_client_options_t", _lib.ClientOptionsC),
                              ("PF", "mogbus_poll_fd_t", _lib.PollFdC),
                              ("CF", "mogbus_client_file_record_t", _lib.ClientFileRecordC)):
            want = header_fields(text, tag)
            have = set()
            for name, _ in cls._fields_:
                have.add(name.rstrip("_"))
                for inner, icls in nested.get(key, {}).items():
                    if name == inner:
                        have.update(f"{inner}.{n.rstrip('_')}" for n, _ in icls._fields_)
            laid_out = {re.sub(r"\[\d+\]", "", k[len(key) + 1:]) for k in c if k.startswith(key + ".")}
            self.assertEqual(want, have, f"{tag}: header and ctypes fields differ")
            self.assertEqual(want, laid_out, f"{tag}: header and layout.c fields differ")


class DeviceTest(unittest.TestCase):
    def setUp(self):
        self.ctx = mogbus.Context()
        self.addCleanup(self.ctx.close)
        self.dev = self.ctx.add_device(1, "unit one")

    def test_devices(self):
        self.assertIs(self.ctx.find_device(1), self.dev)
        self.assertIsNone(self.ctx.find_device(2))
        self.ctx.add_device(2)
        self.assertEqual([d.unit_id for d in self.ctx.devices], [1, 2])
        self.assertEqual([d.unit_id for d in self.ctx], [1, 2])
        self.assertEqual(self.dev.name, "unit one")
        with self.assertRaises(MogbusError):
            self.ctx.add_device(3, "x" * 64)

    def test_any_unit(self):
        other = self.ctx.add_device(2)
        self.assertFalse(self.dev.info().any_unit)
        self.dev.set_any_unit()
        self.assertTrue(self.dev.info().any_unit)
        with self.assertRaises(MogbusError):
            other.set_any_unit(True)
        self.dev.set_any_unit(False)
        other.set_any_unit(True)
        self.assertTrue(other.info().any_unit)
        self.assertFalse(self.dev.info().any_unit)

    def test_points_and_read_write(self):
        d = self.dev
        d.add_point("holding", 0, "u16", 1234)
        d.add_point(mogbus.Space.HOLDING, 1, "f32", 1.5)
        d.add_point("holding", 3, "u32", 0x12345678, word_order="low-first")
        d.add_point("coil", 0, "bool", 1)
        d.add_point("input", 0, "s16", -5)
        self.assertEqual(d.read("holding", 0, 1), [1234])
        self.assertEqual(d.read("holding", 1, 2), [0x3FC0, 0])
        self.assertEqual(d.read("holding", 3, 2), [0x5678, 0x1234])
        self.assertEqual(d.read("coil", 0, 1), [1])
        self.assertEqual(d.get_value("holding", 1), 1.5)
        self.assertEqual(d.get_value("input", 0), -5)
        d.write("holding", 0, [7])
        self.assertEqual(d.get_value("holding", 0), 7)
        d.write("input", 0, 0xFFFB)  # the host plays the physical side
        self.assertEqual(d.get_value("input", 0), -5)
        d.set_value("holding", 1, 2.25)
        self.assertEqual(d.get_value("holding", 1), 2.25)
        self.assertEqual(d.read("holding", 0, 0), [])
        info = d.info()
        self.assertEqual(info.unit_id, 1)
        self.assertEqual(info.name, "unit one")
        self.assertEqual(info.point_counts["holding"], 3)
        self.assertEqual(info.point_counts["coil"], 1)
        self.assertEqual(info.faults, {})
        self.assertEqual(info.custom_function_codes, [])
        pts = d.points()
        self.assertEqual([(q.space, q.address, q.type) for q in pts],
                         [(mogbus.Space.HOLDING, 0, mogbus.ValueType.U16),
                          (mogbus.Space.HOLDING, 1, mogbus.ValueType.F32),
                          (mogbus.Space.HOLDING, 3, mogbus.ValueType.U32),
                          (mogbus.Space.COIL, 0, mogbus.ValueType.BOOL),
                          (mogbus.Space.INPUT, 0, mogbus.ValueType.S16)])
        self.assertEqual(pts[2].word_order, mogbus.WordOrder.LOW_FIRST)

    def test_ramp(self):
        self.dev.add_point("holding", 0, "u16",
                           ramp={"from": 10, "to": 20, "step": 5, "every_ms": 60000, "mode": "hold"})
        self.dev.add_point("holding", 1, "u16", ramp=mogbus.Ramp(1, 9, 2, 1000, "bounce"))
        self.assertEqual(self.dev.get_value("holding", 0), 10)
        self.assertEqual(self.dev.read("holding", 1, 1), [1])
        with self.assertRaises(MogbusError):
            self.dev.add_point("holding", 2, "u16", ramp={"from": 0, "to": 1, "step": 0, "every_ms": 1})
        with self.assertRaisesRegex(MogbusError, "invalid ramp"):
            self.dev.add_point("holding", 2, "u16", ramp={"bogus": 1})

    def test_clock(self):
        self.dev.add_point("holding", 0, "u16", clock="year", clock_utc=True)
        self.dev.add_point("holding", 1, "u32", clock="unix")
        self.assertGreaterEqual(self.dev.get_value("holding", 0), 2024)
        self.assertGreater(self.dev.get_value("holding", 1), 1_700_000_000)
        with self.assertRaisesRegex(MogbusError, "32-bit"):
            self.dev.add_point("holding", 5, "u16", clock="unix")
        with self.assertRaises(MogbusError):
            self.dev.add_point("holding", 6, "float32", clock="hour")

    def test_errors(self):
        d = self.dev
        d.add_point("holding", 0)
        with self.assertRaisesRegex(MogbusError, "invalid space"):
            d.read("nowhere", 0, 1)
        with self.assertRaisesRegex(MogbusError, "invalid type"):
            d.add_point("holding", 5, "f128")
        with self.assertRaises(MogbusError):
            d.add_point("holding", 0)  # overlap: the C message comes through
        with self.assertRaisesRegex(MogbusError, "undefined or out of range"):
            d.read("holding", 100, 1)
        with self.assertRaises(MogbusError):
            d.write("holding", 100, [1])
        with self.assertRaises(MogbusError):
            d.write("holding", 0, [70000])
        with self.assertRaisesRegex(MogbusError, "no point starts"):
            d.get_value("holding", 100)
        with self.assertRaises(MogbusError):
            d.set_value("holding", 100, 1)
        with self.assertRaises(MogbusError):
            d.read("holding", 70000, 1)

    def test_closed_context(self):
        self.ctx.close()
        self.ctx.close()
        with self.assertRaisesRegex(MogbusError, "closed"):
            self.dev.read("holding", 0, 1)
        with self.assertRaisesRegex(MogbusError, "closed"):
            self.ctx.add_device(3)


class DataTypeTest(unittest.TestCase):
    """64-bit types, byte order and strings, checked against Python's struct module."""

    def setUp(self):
        self.ctx = mogbus.Context()
        self.addCleanup(self.ctx.close)
        self.dev = self.ctx.add_device(1)

    @staticmethod
    def registers(raw, word_order, byte_order):
        """The registers for the big-endian bytes `raw` the way the point layouts define them."""
        words = [raw[i:i + 2] for i in range(0, len(raw), 2)]
        if word_order == "low-first":
            words.reverse()
        if byte_order == "little":
            words = [w[::-1] for w in words]
        return [struct.unpack(">H", w)[0] for w in words]

    def test_every_type_word_order_and_byte_order(self):
        cases = [("u16", ">H", 0xBEEF), ("s16", ">h", -1234), ("u32", ">I", 0xDEADBEEF),
                 ("s32", ">i", -123456789), ("f32", ">f", 1.5),
                 ("u64", ">Q", 0x0102030405060000), ("s64", ">q", -0x0102030405060000),
                 ("f64", ">d", -123.456), ("float64", ">d", 2.5)]
        address = 0
        for name, fmt, value in cases:
            for word_order in ("high-first", "low-first"):
                for byte_order in ("big", "little"):
                    # Only 16-bit types ignore the word order, so skip the duplicate.
                    width = struct.calcsize(fmt) // 2
                    self.dev.add_point("holding", address, name, value, word_order=word_order,
                                       byte_order=byte_order)
                    want = self.registers(struct.pack(fmt, value), word_order, byte_order)
                    with self.subTest(type=name, word_order=word_order, byte_order=byte_order):
                        self.assertEqual(self.dev.read("holding", address, width), want)
                        if name in ("u64", "s64"):
                            # Above 2^53 the decoded value is a double; compare as one.
                            self.assertEqual(self.dev.get_value("holding", address), float(value))
                        else:
                            self.assertEqual(self.dev.get_value("holding", address),
                                             struct.unpack(fmt, struct.pack(fmt, value))[0])
                    address += width

    def test_four_common_32_bit_layouts(self):
        # 1.5f = 3F C0 00 00
        for word_order, byte_order, want in (("high-first", "big", [0x3FC0, 0x0000]),   # ABCD
                                              ("low-first", "big", [0x0000, 0x3FC0]),    # CDAB
                                              ("high-first", "little", [0xC03F, 0x0000]),  # BADC
                                              ("low-first", "little", [0x0000, 0xC03F])):  # DCBA
            ctx = mogbus.Context()
            self.addCleanup(ctx.close)
            dev = ctx.add_device(1)
            dev.add_point("input", 0, "f32", 1.5, word_order=word_order, byte_order=byte_order)
            self.assertEqual(dev.read("input", 0, 2), want)

    def test_set_value_and_client_writes_use_the_layout(self):
        d = self.dev
        d.add_point("holding", 0, "f64", 0, word_order="low-first", byte_order="little")
        d.write("holding", 0, self.registers(struct.pack(">d", 99.5), "low-first", "little"))
        self.assertEqual(d.get_value("holding", 0), 99.5)
        d.set_value("holding", 0, -7.25)  # claims the point for the host
        want = self.registers(struct.pack(">d", -7.25), "low-first", "little")
        self.assertEqual(d.read("holding", 0, 4), want)

    def test_sixty_four_bit_ramp(self):
        self.dev.add_point("holding", 0, "u64", ramp={"from": 5, "to": 5, "step": 1,
                                                       "every_ms": 1000, "mode": "hold"})
        self.assertEqual(self.dev.read("holding", 0, 4), [0, 0, 0, 5])

    def test_strings(self):
        d = self.dev
        d.add_point("holding", 0, "string", length=3, text="ABCDE")
        d.add_point("input", 0, "string", length=2, text="AB", byte_order="little")
        d.add_point("holding", 10, "string", length=2)
        self.assertEqual(d.read("holding", 0, 3), [0x4142, 0x4344, 0x4500])
        self.assertEqual(d.read("input", 0, 2), [0x4241, 0x0000])
        self.assertEqual(d.get_string("holding", 0), "ABCDE")
        self.assertEqual(d.get_string("input", 0), "AB")
        self.assertEqual(d.get_string("holding", 10), "")
        # A client write of raw registers updates the text.
        d.write("holding", 0, list(struct.unpack(">3H", b"Hi\0\0\0\0")))
        self.assertEqual(d.get_string("holding", 0), "Hi")
        d.set_string("holding", 0, "XYZ")
        self.assertEqual(d.read("holding", 0, 3), [0x5859, 0x5A00, 0])
        d.write("holding", 0, [0x5859, 0x5A00, 0x1234])  # clients may write anything
        with self.assertRaisesRegex(MogbusError, "longer than its registers"):
            d.set_string("holding", 0, "1234567")
        with self.assertRaises(MogbusError):
            d.get_string("holding", 5)
        with self.assertRaises(MogbusError):
            d.get_value("holding", 0)  # a string has no number
        with self.assertRaises(MogbusError):
            d.set_value("holding", 0, 1)

    def test_string_errors(self):
        d = self.dev
        with self.assertRaisesRegex(MogbusError, "between 1 and 123"):
            d.add_point("holding", 0, "string")
        with self.assertRaisesRegex(MogbusError, "between 1 and 123"):
            d.add_point("holding", 0, "string", length=124)
        with self.assertRaisesRegex(MogbusError, "longer than its registers"):
            d.add_point("holding", 0, "string", length=1, text="abc")
        with self.assertRaisesRegex(MogbusError, "only applies"):
            d.add_point("holding", 0, "u16", text="abc")
        with self.assertRaisesRegex(MogbusError, "invalid byte_order"):
            d.add_point("holding", 0, "u16", byte_order="middle")
        d.add_point("holding", 0, "string", length=123, text="x" * 246)
        self.assertEqual(d.get_string("holding", 0), "x" * 246)


class ConfigTest(unittest.TestCase):
    def test_load_config_and_serve_config(self):
        port = free_port()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "cfg.scm"
            path.write_text(CONFIG.format(port=port))
            with mogbus.Context() as ctx:
                self.assertIsNone(ctx.config_server_options())
                with self.assertRaises(MogbusError) as caught:
                    ctx.serve_config()
                self.assertEqual(caught.exception.status, mogbus.Status.INVALID)
                ctx.load_config(path)
                self.assertEqual(ctx.config_server_options().port, port)
                self.assertEqual(ctx.find_device(7).get_value("holding", 0), 42)
                with ctx.serve_config().start() as server:
                    with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
                        self.assertEqual(mbap_read(sock, 7, 0), [42])
                    self.assertTrue(mogbus.probe(ctx.config_server_options(), 7))
                    self.assertTrue(server.closed is False)
                self.assertTrue(server.closed)
                with self.assertRaises(MogbusError):
                    ctx.load_config(Path(tmp) / "missing.scm")

    def test_load_config_error_carries_the_loader_message(self):
        with mogbus.Context() as ctx, tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "bad.scm"
            bad.write_text("(device (unit-id 1) (holding 0 (statik 1)))")
            with self.assertRaisesRegex(MogbusError, "unknown option 'statik'") as caught:
                ctx.load_config(bad)
            self.assertEqual(caught.exception.status, mogbus.Status.CONFIG)
            self.assertEqual(len(ctx.devices), 0)

    def test_check_server_and_status(self):
        with mogbus.Context() as ctx:
            ctx.add_device(1).add_point("holding", 0)
            ctx.check_server(mode="udp", port=0)
            with self.assertRaises(MogbusError):
                ctx.check_server(mode="rtu-device")  # needs a path
            blocker = socket.socket()
            blocker.bind(("127.0.0.1", 0))
            blocker.listen()
            self.addCleanup(blocker.close)
            with self.assertRaises(MogbusError) as caught:
                ctx.serve(port=blocker.getsockname()[1])
            self.assertEqual(caught.exception.status, mogbus.Status.LISTEN)


class ServerTest(unittest.TestCase):
    def test_tcp_with_background_thread(self):
        port = free_port()
        with mogbus.Context() as ctx:
            dev = ctx.add_device(1)
            dev.add_point("holding", 0, "u16", 100)
            dev.add_point("holding", 1, "u16", 5)
            server = ctx.serve(port=port).start()
            with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
                self.assertEqual(mbap_read(sock, 1, 0, 2), [100, 5])
                dev.set_value("holding", 0, 200)
                self.assertEqual(mbap_read(sock, 1, 0), [200])
                dev.write("holding", 1, [300])  # register 0 is host-owned after set_value
                self.assertEqual(mbap_read(sock, 1, 1), [300])
                self.assertEqual(mbap_read(sock, 1, 50), ("exception", 2))
                self.assertEqual(mbap_read(sock, 9, 0), ("exception", 11))
            text = server.metrics()
            self.assertIn("mogbus_requests_total", text)
            self.assertIsNone(server.pty_name)
            with self.assertRaisesRegex(MogbusError, "already started"):
                server.start()
            server.close()
            server.close()
            with self.assertRaisesRegex(MogbusError, "closed"):
                server.step()
            self.assertIsNone(server._thread)

    def test_udp(self):
        port = free_port(socket.SOCK_DGRAM)
        with mogbus.Context() as ctx:
            ctx.add_device(1).add_point("holding", 0, "u16", 77)
            with ctx.serve(mode="udp", port=port).start():
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                    s.settimeout(2)
                    s.sendto(struct.pack(">HHHBBHH", 1, 0, 6, 1, 3, 0, 1), ("127.0.0.1", port))
                    reply, _ = s.recvfrom(256)
                self.assertEqual(struct.unpack(">H", reply[9:11])[0], 77)

    def test_manual_step_and_run(self):
        port = free_port()
        with mogbus.Context() as ctx:
            ctx.add_device(1).add_point("holding", 0, "u16", 9)
            server = ctx.serve(port=port)
            self.assertEqual(server.step(0), 0)
            runner = threading.Thread(target=server.run)
            runner.start()
            with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
                self.assertEqual(mbap_read(sock, 1, 0), [9])
            server.stop()
            runner.join(5)
            self.assertFalse(runner.is_alive())

    def test_probe(self):
        port = free_port()
        with mogbus.Context() as ctx:
            ctx.add_device(1).add_point("holding", 0)
            options = ServerOptions(port=port)
            with ctx.serve(options).start():
                self.assertTrue(mogbus.probe(options, 1, timeout_ms=2000))
                mogbus.probe_or_raise(options, 1)
        self.assertFalse(mogbus.probe(options, 1, timeout_ms=300))
        with self.assertRaises(MogbusError):
            mogbus.probe_or_raise(options, 1, timeout_ms=300)

    def test_two_servers_with_ids(self):
        p1, p2 = free_port(), free_port()
        with mogbus.Context() as ctx:
            d1 = ctx.add_device(1)
            d1.add_point("holding", 0, "u16", 11)
            d1.add_server("a")
            d2 = ctx.add_device(1)
            d2.add_point("holding", 0, "u16", 22)
            d2.add_server("b")
            with self.assertRaisesRegex(MogbusError, "unit id"):
                ctx.add_device(1).add_server("a")
            with ctx.serve(ServerOptions(port=p1, id="a")).start(), \
                    ctx.serve(ServerOptions(port=p2, id="b")).start():
                for port, value in ((p1, 11), (p2, 22)):
                    with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
                        self.assertEqual(mbap_read(sock, 1, 0), [value])

    def test_rtu_timing(self):
        t35, gap = mogbus.rtu_timing(9600)
        self.assertGreater(t35, 0)
        self.assertEqual(mogbus.rtu_timing(0), (0, 20))
        self.assertEqual(mogbus.rtu_timing(9600, 7).gap_ms, 7)
        self.assertGreater(gap, 0)

    def test_invalid_options(self):
        with mogbus.Context() as ctx:
            ctx.add_device(1).add_point("holding", 0)
            with self.assertRaisesRegex(MogbusError, "invalid mode"):
                ctx.serve(mode="carrier-pigeon")
            with self.assertRaisesRegex(MogbusError, "invalid server option"):
                ctx.serve(nonsense=1)
            with self.assertRaises(MogbusError):
                ctx.serve(port=70000)
            self.assertEqual(len(ctx), 1)

    def test_close_context_joins_threads(self):
        ctx = mogbus.Context()
        ctx.add_device(1).add_point("holding", 0)
        servers = [ctx.serve(port=free_port()).start() for _ in range(2)]
        threads = [s._thread for s in servers]
        ctx.close()
        for t in threads:
            self.assertFalse(t.is_alive())
        self.assertTrue(all(s.closed for s in servers))


class LogTest(unittest.TestCase):
    def tearDown(self):
        mogbus.set_log_sink(None)
        mogbus.set_log_format("text")
        mogbus.set_log_level("info")

    def test_json_sink(self):
        lines = []
        mogbus.set_log_sink(lambda level, msg: lines.append((level, msg)))
        mogbus.set_log_format("json")
        mogbus.set_log_level("debug")
        port = free_port()
        with mogbus.Context() as ctx:
            ctx.add_device(1).add_point("holding", 0, "u16", 3)
            with ctx.serve(port=port).start():
                with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
                    mbap_read(sock, 1, 0)
        self.assertTrue(lines)
        parsed = [json.loads(msg) for _, msg in lines]
        self.assertTrue(all("level" in p and "msg" in p for p in parsed))
        self.assertTrue(any(p.get("event") for p in parsed))
        with self.assertRaises(MogbusError):
            mogbus.set_log_level("loud")


class ControlApiTest(unittest.TestCase):
    def test_http_control(self):
        import urllib.request
        http_port = free_port()
        with mogbus.serve(config_text=DEVICES, port=0, http_bind_address="127.0.0.1",
                          http_port=http_port, http_control=True) as srv:
            base = f"http://127.0.0.1:{http_port}/api/devices"
            with urllib.request.urlopen(base, timeout=5) as r:
                devices = json.load(r)
            self.assertEqual(devices[0]["unit"], 7)
            self.assertEqual(devices[0]["name"], "cfg")
            srv.device(7).set_value("holding", 0, 55)
            with urllib.request.urlopen(base + "/7/holding/0", timeout=5) as r:
                self.assertEqual(json.load(r), {"values": [55]})


    def test_http_ui(self):
        import urllib.error
        import urllib.request
        http_port = free_port()
        with mogbus.serve(config_text=DEVICES, port=0, http_bind_address="127.0.0.1",
                          http_port=http_port, http_ui=True,
                          http_allowed_hosts=["sim.lab", "mogbus"]) as srv:
            base = f"http://127.0.0.1:{http_port}"
            with urllib.request.urlopen(base + "/", timeout=5) as r:
                page = r.read().decode()
                self.assertTrue(r.headers["Content-Type"].startswith("text/html"))
                self.assertIn("frame-ancestors 'none'", r.headers["Content-Security-Policy"])
            self.assertTrue(page.startswith("<!doctype html>"))
            # http_ui implies the control API it uses.
            with urllib.request.urlopen(base + "/api/devices", timeout=5) as r:
                self.assertEqual(json.load(r)[0]["unit"], 7)
            for host, ok in (("sim.lab", True), ("MOGBUS:80", True), ("evil.example", False)):
                req = urllib.request.Request(base + "/api/devices", headers={"Host": host})
                if ok:
                    urllib.request.urlopen(req, timeout=5).close()
                else:
                    with self.assertRaises(urllib.error.HTTPError) as cm:
                        urllib.request.urlopen(req, timeout=5)
                    self.assertEqual(cm.exception.code, 403)
        with self.assertRaisesRegex(MogbusError, "allowed hosts"):
            with mogbus.serve(config_text=DEVICES, port=0, http_bind_address="127.0.0.1",
                              http_port=free_port(), http_allowed_hosts="a b"):
                pass


class ServeHelperTest(unittest.TestCase):
    def test_point_notes(self):
        with mogbus.Context() as ctx:
            dev = ctx.add_device(1)
            dev.add_point("holding", 0, "u16", 5, note="Setpoint")
            dev.add_point("holding", 1, "u16", 6)
            self.assertEqual([p.note for p in dev.points()], ["Setpoint", ""])
            with self.assertRaisesRegex(MogbusError, "note"):
                dev.add_point("holding", 2, "u16", 6, note="x" * 128)

    def test_read_only_points(self):
        with mogbus.Context() as ctx:
            dev = ctx.add_device(1)
            dev.add_point("holding", 0, "u16", 5, read_only=True)
            dev.add_point("holding", 1, "u16", 6)
            dev.add_point("coil", 0, "bool", 1, read_only=True)
            with self.assertRaisesRegex(MogbusError, "read-only"):
                dev.add_point("input", 0, "u16", 1, read_only=True)
            dev.write("holding", 0, 9)  # the host may write read-only points
            dev.set_value("holding", 0, 10)
            self.assertEqual(dev.read("holding", 0, 1), [10])
            port = free_port()
            options = mogbus.ServerOptions(mode="tcp", port=port)
            with ctx.serve(options).start() as server:
                with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
                    def write(fc, *words):
                        sock.sendall(struct.pack(">HHHBB%dH" % len(words), 1, 0, 2 + 2 * len(words), 1, fc, *words))
                        reply = sock.recv(256)
                        return reply[8] if reply[7] & 0x80 else None

                    self.assertEqual(write(6, 0, 1), 2)
                    self.assertEqual(write(6, 1, 77), None)
                    self.assertEqual(write(5, 0, 0), 2)
                    self.assertEqual(mbap_read(sock, 1, 0, 2), [10, 77])
                self.assertEqual(dev.read("coil", 0, 1), [1])

    def test_serve_config_text(self):
        with mogbus.serve(config_text=DEVICES, port=0) as srv:
            self.assertGreater(srv.port, 0)
            with socket.create_connection(("127.0.0.1", srv.port), timeout=2) as sock:
                self.assertEqual(mbap_read(sock, 7, 0), [42])
                srv.device(7).set_value("holding", 0, 43)
                self.assertEqual(mbap_read(sock, 7, 0), [43])
            thread = srv.server._thread
            with self.assertRaises(MogbusError):
                srv.device(99)
        self.assertFalse(thread.is_alive())
        self.assertTrue(srv.context.closed)

    def test_serve_config_file_udp(self):
        with tempfile.NamedTemporaryFile("w", suffix=".scm") as f:
            f.write(DEVICES)
            f.flush()
            with mogbus.serve(config=f.name, mode="udp", port=0) as srv:
                self.assertTrue(mogbus.probe(ServerOptions(mode="udp", port=srv.port), 7))

    def test_serve_uses_config_server_section(self):
        port = free_port(socket.SOCK_DGRAM)
        text = "(server (mode udp) (port %d))\n" % port + DEVICES
        with mogbus.serve(config_text=text) as srv:
            self.assertEqual(srv.port, port)
            self.assertTrue(mogbus.probe(ServerOptions(mode="udp", port=port), 7))
        with mogbus.serve(config_text=text, port=0) as srv:
            self.assertNotEqual(srv.port, port)
            self.assertGreater(srv.port, 0)
            self.assertTrue(mogbus.probe(ServerOptions(mode="udp", port=srv.port), 7))
        tcp_port = free_port()
        with mogbus.serve(config_text=text, mode="tcp", port=tcp_port) as srv:  # override wins
            self.assertEqual(srv.port, tcp_port)
            with socket.create_connection(("127.0.0.1", tcp_port), timeout=2) as sock:
                self.assertEqual(mbap_read(sock, 7, 0), [42])

    def test_serve_errors(self):
        with self.assertRaises(MogbusError):
            with mogbus.serve():  # no devices
                pass
        with self.assertRaises(MogbusError):
            with mogbus.serve(config_text="(device", ):
                pass


class AccessHookTest(unittest.TestCase):
    def raw_write(self, sock, unit, address, value):
        """FC 06; returns the echoed value or ("exception", code)."""
        sock.sendall(struct.pack(">HHHBBHH", 2, 0, 6, unit, 6, address, value))
        reply = sock.recv(256)
        if reply[7] & 0x80:
            return ("exception", reply[8])
        return struct.unpack(">H", reply[10:12])[0]

    def test_hook_refreshes_values_and_calls_back_into_the_device(self):
        port = free_port()
        seen = []
        with mogbus.Context() as ctx:
            dev = ctx.add_device(1)
            dev.add_point("holding", 0, "u16", 1)
            dev.add_point("holding", 1, "u16", 2)

            def hook(access):
                seen.append(access)
                if access.kind == mogbus.AccessKind.READ:
                    dev.set_value("holding", 0, 500 + len(seen))  # reentrant: the lock is an RLock
                    dev.get_value("holding", 1)

            dev.set_access_hook(hook)
            with ctx.serve(port=port).start():
                with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
                    self.assertEqual(mbap_read(sock, 1, 0, 2), [501, 2])
                    self.assertEqual(mbap_read(sock, 1, 0), [502])
                    self.assertEqual(seen[0].kind, mogbus.AccessKind.READ)
                    self.assertEqual(
                        (seen[0].space, seen[0].address, seen[0].count, seen[0].values, seen[0].function_code),
                        (mogbus.Space.HOLDING, 0, 2, None, 3))
                    self.assertEqual(self.raw_write(sock, 1, 1, 77), 77)
                    self.assertEqual(seen[-1].kind, mogbus.AccessKind.WRITE)
                    self.assertEqual((seen[-1].values, seen[-1].function_code), ([77], 6))
                    dev.set_access_hook(None)
                    n = len(seen)
                    self.assertEqual(mbap_read(sock, 1, 0), [502])
                    self.assertEqual(len(seen), n)

    def test_written_hook_after_client_writes_only(self):
        port = free_port()
        seen = []
        with mogbus.Context() as ctx:
            dev = ctx.add_device(1)
            dev.add_point("holding", 0, "u16", 1)
            dev.add_point("holding", 1, "u16", 0)
            mode = {"v": "ok"}

            def written(access):
                seen.append((access.kind, access.function_code, access.address, access.values))
                if mode["v"] == "raise":
                    raise RuntimeError("boom")
                dev.set_value("holding", 1, access.values[0] + 1)  # reentrant host write, no recursion

            def veto(access):
                return 6 if access.kind == mogbus.AccessKind.WRITE and access.values[0] == 99 else None

            dev.set_written_hook(written)
            dev.set_access_hook(veto)
            with ctx.serve(port=port).start():
                with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
                    self.assertEqual(self.raw_write(sock, 1, 0, 7), 7)
                    self.assertEqual(seen, [(mogbus.AccessKind.WRITE, 6, 0, [7])])
                    self.assertEqual(dev.get_value("holding", 1), 8)
                    self.assertEqual(self.raw_write(sock, 1, 0, 99), ("exception", 6))
                    self.assertEqual(len(seen), 1)
                    dev.write("holding", 0, [5])  # host write: not reported
                    self.assertEqual(len(seen), 1)
                    mode["v"] = "raise"
                    self.assertEqual(self.raw_write(sock, 1, 0, 3), 3)  # raising hook: write stays
                    self.assertEqual(len(seen), 2)
                    dev.set_written_hook(None)
                    self.assertEqual(self.raw_write(sock, 1, 0, 4), 4)
                    self.assertEqual(len(seen), 2)

    def test_frame_hook_sees_rewrites_and_drops_frames(self):
        port = free_port()
        seen = []
        rule = {"rx": None, "tx": None}
        with mogbus.Context() as ctx:
            dev = ctx.add_device(1)
            dev.add_point("holding", 0, "u16", 0x1111)
            dev.add_point("holding", 1, "u16", 0x2222)

            def hook(frame):
                seen.append(frame)
                action = rule["rx" if frame.direction == mogbus.FrameDirection.RECEIVED else "tx"]
                return action(frame) if action else None

            with ctx.serve(port=port) as server:
                server.start()
                server.set_frame_hook(hook)
                with socket.create_connection(("127.0.0.1", port), timeout=0.5) as sock:
                    self.assertEqual(mbap_read(sock, 1, 0), [0x1111])
                    rx, tx = seen
                    self.assertEqual((rx.direction, tx.direction), (mogbus.FrameDirection.RECEIVED,
                                                                    mogbus.FrameDirection.SENT))
                    self.assertEqual((rx.mode, rx.capacity), (mogbus.Mode.TCP, 260))
                    self.assertTrue(rx.peer.startswith("127.0.0.1:"))
                    self.assertEqual(rx.data[7:], bytes([3, 0, 0, 0, 1]))
                    self.assertEqual(tx.data[7:], bytes([3, 2, 0x11, 0x11]))

                    rule["rx"] = lambda f: f.data[:9] + b"\x01" + f.data[10:]  # read address 1 instead
                    self.assertEqual(mbap_read(sock, 1, 0), [0x2222])
                    rule["rx"] = None
                    rule["tx"] = lambda f: f.data[:-1] + b"\x99"
                    self.assertEqual(mbap_read(sock, 1, 0), [0x1199])

                    # b"" drops, so does a replacement longer than the capacity; the connection survives.
                    for kind, action in (("rx", lambda f: b""), ("tx", lambda f: b""),
                                         ("rx", lambda f: bytes(f.capacity + 1)),
                                         ("tx", lambda f: bytes(f.capacity + 1))):
                        rule["rx"] = rule["tx"] = None
                        rule[kind] = action
                        with self.assertRaises(socket.timeout):
                            mbap_read(sock, 1, 0)
                    rule["rx"] = rule["tx"] = None

                    # A raising hook lets the frame through.
                    def boom(frame):
                        raise RuntimeError("boom")

                    rule["rx"] = boom
                    with contextlib.redirect_stderr(io.StringIO()):
                        self.assertEqual(mbap_read(sock, 1, 0), [0x1111])
                    rule["rx"] = None

                    server.set_frame_hook(None)
                    n = len(seen)
                    self.assertEqual(mbap_read(sock, 1, 0), [0x1111])
                    self.assertEqual(len(seen), n)

    def test_connection_hook_reports_open_and_close(self):
        port = free_port()
        seen = []
        with mogbus.Context() as ctx:
            dev = ctx.add_device(1)
            dev.add_point("holding", 0, "u16", 0x1111)
            with ctx.serve(port=port) as server:
                server.start()
                server.set_connection_hook(seen.append)
                with socket.create_connection(("127.0.0.1", port), timeout=0.5) as sock:
                    self.assertEqual(mbap_read(sock, 1, 0), [0x1111])
                    local = sock.getsockname()
                deadline = time.time() + 2
                while len(seen) < 2 and time.time() < deadline:
                    time.sleep(0.01)
                opened, closed = seen
                self.assertTrue(opened.connected)
                self.assertEqual((opened.mode, opened.reason), (mogbus.Mode.TCP, mogbus.CloseReason.NONE))
                self.assertEqual(opened.peer, f"127.0.0.1:{local[1]}")
                self.assertFalse(closed.connected)
                self.assertEqual((closed.id, closed.peer), (opened.id, opened.peer))
                self.assertEqual(closed.reason, mogbus.CloseReason.PEER)

                # A client still connected when the server is closed gets a SERVER_STOP close.
                with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                    deadline = time.time() + 2
                    while len(seen) < 3 and time.time() < deadline:
                        time.sleep(0.01)
                    self.assertEqual(len(seen), 3)
                    self.assertNotEqual(seen[2].id, opened.id)
                    server.close()
                self.assertEqual(len(seen), 4)
                self.assertEqual((seen[3].id, seen[3].reason), (seen[2].id, mogbus.CloseReason.SERVER_STOP))

    def test_connection_hook_exception_is_printed(self):
        port = free_port()
        with mogbus.Context() as ctx:
            ctx.add_device(1)
            with ctx.serve(port=port) as server:
                server.start()

                def boom(_event):
                    raise RuntimeError("boom")

                server.set_connection_hook(boom)
                err = io.StringIO()
                with contextlib.redirect_stderr(err):
                    with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                        deadline = time.time() + 2
                        while "boom" not in err.getvalue() and time.time() < deadline:
                            time.sleep(0.01)
                self.assertIn("RuntimeError: boom", err.getvalue())
                server.set_connection_hook(None)

    def test_hook_veto_and_raising_hook(self):
        port = free_port()
        with mogbus.Context() as ctx:
            dev = ctx.add_device(1)
            dev.add_point("holding", 0, "u16", 1)
            mode = {"v": "veto"}

            def hook(access):
                if mode["v"] == "raise":
                    raise RuntimeError("boom")
                return 6 if access.kind == mogbus.AccessKind.WRITE else None

            dev.set_access_hook(hook)
            with ctx.serve(port=port).start():
                with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
                    self.assertEqual(self.raw_write(sock, 1, 0, 9), ("exception", 6))
                    self.assertEqual(dev.read("holding", 0), [1])
                    mode["v"] = "raise"
                    with contextlib.redirect_stderr(io.StringIO()):
                        self.assertEqual(mbap_read(sock, 1, 0), ("exception", 4))
                        self.assertEqual(self.raw_write(sock, 1, 0, 9), ("exception", 4))
                    self.assertEqual(dev.read("holding", 0), [1])


def mbap_pdu(sock, unit, pdu):
    """Sends one PDU over MBAP and returns the response PDU."""
    sock.sendall(struct.pack(">HHHB", 1, 0, len(pdu) + 1, unit) + pdu)
    reply = b""
    while len(reply) < 8:
        chunk = sock.recv(512)
        if not chunk:
            raise EOFError("connection closed")
        reply += chunk
    length = struct.unpack(">H", reply[4:6])[0]
    while len(reply) < 6 + length:
        reply += sock.recv(512)
    return reply[7:6 + length]


class FunctionCodeTest(unittest.TestCase):
    def test_reply_exception_silence_and_errors(self):
        port = free_port()
        with mogbus.Context() as ctx:
            dev = ctx.add_device(1)
            dev.add_point("holding", 0, "u16", value=7)

            def status(request):
                if request == b"\x00":
                    return None
                if request == b"\x01":
                    return 6
                if request == b"\x02":
                    raise RuntimeError("boom")
                if request == b"\x03":
                    return b"x" * 253
                return bytes(reversed(request)) + bytes([int(dev.get_value("holding", 0))])  # reentrant

            dev.add_function_code(0x41, status)
            self.assertEqual([c.code for c in dev.info().custom_function_codes], [0x41])
            with self.assertRaises(mogbus.MogbusError):
                dev.add_function_code(0x41, status)
            with self.assertRaises(mogbus.MogbusError):
                dev.add_function_code(0x03, status)
            with self.assertRaises(mogbus.MogbusError):
                dev.add_function_code(0, status)
            with ctx.serve(port=port).start():
                with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
                    self.assertEqual(mbap_pdu(sock, 1, bytes([0x41, 1, 2])), bytes([0x41, 2, 1, 7]))
                    self.assertEqual(mbap_pdu(sock, 1, bytes([0x41, 1])), bytes([0xC1, 6]))
                    with contextlib.redirect_stderr(io.StringIO()):
                        self.assertEqual(mbap_pdu(sock, 1, bytes([0x41, 2])), bytes([0xC1, 4]))
                        self.assertEqual(mbap_pdu(sock, 1, bytes([0x41, 3])), bytes([0xC1, 4]))
                    sock.settimeout(0.3)
                    with self.assertRaises(socket.timeout):
                        mbap_pdu(sock, 1, bytes([0x41, 0]))   # no response


class AccessTargetsTest(unittest.TestCase):
    """The host API and the access hook for file records, FIFO queues and CANopen objects."""

    def device(self, ctx):
        dev = ctx.add_device(1)
        dev.add_file_record(1, 0, [10, 20, 30])
        dev.add_file_record(2, 5, [40])
        dev.add_fifo(0x100, [1, 2, 3])
        dev.add_canopen_object(5, 0x6041, 0, b"\x01\x02\x03\x04")
        return dev

    def test_host_functions(self):
        with mogbus.Context() as ctx:
            dev = self.device(ctx)
            self.assertEqual(dev.read_file_records(1, 1, 2), [20, 30])
            dev.write_file_records(1, 0, [7, 8])
            dev.write_file_records(2, 5, 9)
            self.assertEqual(dev.read_file_records(1, 0, 3), [7, 8, 30])
            self.assertEqual(dev.read_file_records(2, 5), [9])
            with self.assertRaises(MogbusError):
                dev.write_file_records(1, 2, [1, 2])  # record 3 is undefined
            self.assertEqual(dev.read_file_records(1, 2), [30])
            with self.assertRaises(MogbusError):
                dev.add_file_record(1, 2, [5])  # overlaps
            dev.set_fifo(0x100, [5, 6])
            dev.set_fifo(0x100)
            with self.assertRaises(MogbusError):
                dev.set_fifo(0x101, [1])
            with self.assertRaises(MogbusError):
                dev.set_fifo(0x100, range(32))
            with self.assertRaises(MogbusError):
                dev.add_fifo(0x100)
            dev.write_canopen(5, 0x6041, 0, b"\xAA\xBB", offset=1)
            self.assertEqual(dev.read_canopen(5, 0x6041, 0, 4), b"\x01\xAA\xBB\x04")
            with self.assertRaises(MogbusError):
                dev.write_canopen(5, 0x6041, 0, b"\x00\x00", offset=3)
            with self.assertRaises(MogbusError):
                dev.read_canopen(5, 0x6041, 1, 1)
            with self.assertRaises(MogbusError):
                dev.add_canopen_object(5, 0x6041, 0, b"\x00")

    def serve_and_run(self, ctx, fn):
        port = free_port()
        with ctx.serve(port=port).start():
            with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
                fn(sock)

    def test_file_record_hook(self):
        seen = []
        with mogbus.Context() as ctx:
            dev = self.device(ctx)

            def hook(access):
                seen.append(access)
                if access.target != mogbus.AccessTarget.FILE_RECORD:
                    return None
                if access.kind == mogbus.AccessKind.READ:
                    dev.write_file_records(access.file, access.address, 99)  # refresh
                elif access.address == 1:
                    return 6  # veto the second sub-request

            dev.set_access_hook(hook)

            def run(sock):
                read = bytes([0x14, 14, 6, 0, 1, 0, 1, 0, 1, 6, 0, 2, 0, 5, 0, 1])
                self.assertEqual(mbap_pdu(sock, 1, read),
                                 bytes([0x14, 8, 3, 6, 0, 99, 3, 6, 0, 99]))
                self.assertEqual([(a.kind, a.file, a.address, a.count, a.values) for a in seen],
                                 [(mogbus.AccessKind.READ, 1, 1, 1, None),
                                  (mogbus.AccessKind.READ, 2, 5, 1, None)])
                seen.clear()
                write = bytes([0x15, 18, 6, 0, 1, 0, 0, 0, 1, 0, 77, 6, 0, 1, 0, 1, 0, 1, 0, 88])
                self.assertEqual(mbap_pdu(sock, 1, write), bytes([0x95, 6]))
                self.assertEqual([(a.address, a.values) for a in seen], [(0, [77]), (1, [88])])
                self.assertEqual(dev.read_file_records(1, 0), [10])  # nothing written
            self.serve_and_run(ctx, run)

    def test_fifo_hook(self):
        with mogbus.Context() as ctx:
            dev = self.device(ctx)
            seen = []

            def hook(access):
                seen.append(access)
                if access.target == mogbus.AccessTarget.FIFO:
                    dev.set_fifo(access.address, [7, 8])

            dev.set_access_hook(hook)

            def run(sock):
                self.assertEqual(mbap_pdu(sock, 1, bytes([0x18, 1, 0])),
                                 bytes([0x18, 0, 6, 0, 2, 0, 7, 0, 8]))
                self.assertEqual((seen[0].target, seen[0].address, seen[0].function_code),
                                 (mogbus.AccessTarget.FIFO, 0x100, 0x18))
            self.serve_and_run(ctx, run)

    def test_canopen_hook(self):
        with mogbus.Context() as ctx:
            dev = self.device(ctx)
            seen = []
            veto = {"on": False}

            def hook(access):
                seen.append(access)
                if access.target != mogbus.AccessTarget.CANOPEN:
                    return None
                if access.kind == mogbus.AccessKind.READ:
                    dev.write_canopen(access.node, access.index, access.subindex, b"\xAB")
                elif veto["on"]:
                    return 2

            dev.set_access_hook(hook)

            def run(sock):
                read = bytes([0x2B, 0x0D, 0, 0, 5, 0x60, 0x41, 0, 0, 0, 0, 2])
                self.assertEqual(mbap_pdu(sock, 1, read), read + b"\xAB\x02")
                a = seen[0]
                self.assertEqual((a.target, a.kind, a.node, a.index, a.subindex, a.address, a.count, a.data),
                                 (mogbus.AccessTarget.CANOPEN, mogbus.AccessKind.READ, 5, 0x6041, 0, 0, 2, None))
                write = bytes([0x2B, 0x0D, 1, 0, 5, 0x60, 0x41, 0, 0, 2, 0, 2, 0x11, 0x22])
                veto["on"] = True
                self.assertEqual(mbap_pdu(sock, 1, write),
                                 bytes([0xAB, 0xFF, 0x00, 0x06, 0x0D, 0xCE, 0x20, 0x00, 0x00, 0x08]))
                self.assertEqual((seen[-1].kind, seen[-1].address, seen[-1].data),
                                 (mogbus.AccessKind.WRITE, 2, b"\x11\x22"))
                self.assertEqual(dev.read_canopen(5, 0x6041, 0, 4), b"\xAB\x02\x03\x04")
                veto["on"] = False
                self.assertEqual(mbap_pdu(sock, 1, write)[:2], bytes([0x2B, 0x0D]))
                self.assertEqual(dev.read_canopen(5, 0x6041, 0, 4), b"\xAB\x02\x11\x22")
            self.serve_and_run(ctx, run)


class LockTest(unittest.TestCase):
    def test_concurrent_access_while_serving(self):
        with mogbus.serve(config_text=DEVICES, port=0) as srv:
            dev = srv.device(7)
            errors = []

            def worker():
                try:
                    for i in range(200):
                        dev.set_value("holding", 0, i)
                        dev.get_value("holding", 0)
                except Exception as exc:  # pragma: no cover
                    errors.append(exc)

            threads = [threading.Thread(target=worker) for _ in range(3)]
            with socket.create_connection(("127.0.0.1", srv.port), timeout=2) as sock:
                for t in threads:
                    t.start()
                for _ in range(50):
                    self.assertEqual(len(mbap_read(sock, 7, 0)), 1)
            for t in threads:
                t.join(10)
                self.assertFalse(t.is_alive())
            self.assertEqual(errors, [])


if __name__ == "__main__":
    unittest.main()
