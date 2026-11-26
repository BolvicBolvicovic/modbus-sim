import contextlib
import ctypes
import json
import os
import socket
import struct
import tempfile
import threading
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
        for key in ("faults_count", "space_count"):
            self.assertIn(key, c)
        self.assertEqual(c.pop("faults_count"), (7,))
        self.assertEqual(c.pop("space_count"), (4,))
        self.assertEqual(c, expected)


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
            self.ctx.add_device(1)

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

    def test_errors(self):
        d = self.dev
        d.add_point("holding", 0)
        with self.assertRaisesRegex(MogbusError, "invalid space"):
            d.read("nowhere", 0, 1)
        with self.assertRaisesRegex(MogbusError, "invalid type"):
            d.add_point("holding", 5, "f64")
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


class ServeHelperTest(unittest.TestCase):
    def test_serve_config_text(self):
        with mogbus.serve(config_text=DEVICES) as srv:
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
            with mogbus.serve(config=f.name, mode="udp") as srv:
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


class LockTest(unittest.TestCase):
    def test_concurrent_access_while_serving(self):
        with mogbus.serve(config_text=DEVICES) as srv:
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
