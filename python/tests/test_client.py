"""mogbus.Client against a server of the same process, started on a background thread."""
import socket
import struct
import threading
import time
import unittest

import mogbus
from mogbus import Client, MogbusError, ModbusException, Status


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class ClientTest(unittest.TestCase):
    def serve(self, mode="tcp"):
        ctx = mogbus.Context()
        self.addCleanup(ctx.close)
        dev = ctx.add_device(1, "client-test")
        for i in range(10):
            dev.add_point("holding", i, "u16", 100 + i)
        for i in range(4):
            dev.add_point("input", i, "u16", 200 + i)
        for i in range(16):
            dev.add_point("coil", i, "bool", i & 1)
        for i in range(8):
            dev.add_point("discrete", i, "bool", 1 - (i & 1))
        dev.add_point("holding", 20, "u16", 0x00F0)
        dev.add_file_record(1, 0, [10, 11, 12, 13, 14])
        dev.add_fifo(100, [0x1234, 0x5678, 42])
        dev.add_canopen_object(5, 0x2000, 0, b"\xDE\xAD\xBE\xEF")
        dev.add_function_code(0x41, lambda request: request[::-1])
        dev.add_function_code(0x43, lambda request: 5)
        dev.add_function_code(0x42, lambda request: None)
        kind = socket.SOCK_STREAM if mode.endswith("tcp") or mode == "tcp" else socket.SOCK_DGRAM
        port = free_port(kind)
        server = ctx.serve(mode=mode, port=port).start()
        self.addCleanup(server.close)
        return port

    def test_every_call_in_every_network_mode(self):
        for mode in ("tcp", "udp", "rtu-tcp", "rtu-udp", "ascii-tcp", "ascii-udp"):
            with self.subTest(mode=mode):
                port = self.serve(mode)
                with Client(mode=mode, port=port, timeout_ms=500) as c:
                    self.assertEqual(c.read_holding_registers(0, 3), [100, 101, 102])
                    self.assertEqual(c.read_input_registers(1, 2), [201, 202])
                    self.assertEqual(c.read_coils(0, 4), [False, True, False, True])
                    self.assertEqual(c.read_discrete_inputs(0, 3), [True, False, True])
                    c.write_coil(0, True)
                    self.assertEqual(c.read_coils(0, 2), [True, True])
                    c.write_register(0, 0xBEEF)
                    self.assertEqual(c.read_holding_registers(0, 1), [0xBEEF])
                    c.write_coils(2, [True, False, True])
                    self.assertEqual(c.read_coils(2, 3), [True, False, True])
                    c.write_registers(5, [1, 2, 3])
                    self.assertEqual(c.read_holding_registers(5, 3), [1, 2, 3])
                    c.mask_write_register(20, 0x00FF, 0x0F00)
                    self.assertEqual(c.read_holding_registers(20, 1), [0x0FF0])
                    self.assertEqual(c.read_write_registers(5, 2, 5, [7, 8]), [7, 8])
                    self.assertIsInstance(c.read_device_id(), dict)
                    self.assertEqual(c.request(0x41, b"\x01\x02\x03"), b"\x03\x02\x01")

    def test_remaining_calls_in_every_network_mode(self):
        for mode in ("tcp", "udp", "rtu-tcp", "rtu-udp", "ascii-tcp", "ascii-udp"):
            with self.subTest(mode=mode):
                port = self.serve(mode)
                with Client(mode=mode, port=port, timeout_ms=500) as c:
                    self.assertEqual(c.read_exception_status(), 0)
                    self.assertEqual(c.diagnostics(0x0000, b"\xAB\xCD\xEF\x01\x23\x45"), b"\xAB\xCD\xEF\x01\x23\x45")
                    self.assertEqual(c.diagnostics(0x000A, b"\0\0"), b"\0\0")
                    with self.assertRaises(ModbusException) as cm:
                        c.diagnostics(0x0003, b"\x0A\0")
                    self.assertEqual(cm.exception.code, 3)
                    self.assertEqual(c.get_comm_event_counter().status, 0)
                    log = c.get_comm_event_log()
                    self.assertEqual((log.status, log.events), (0, b""))
                    self.assertEqual(c.report_server_id(), b"\x00\xFF")
                    self.assertEqual(c.read_file_records([(1, 0, 2), (1, 4, 1)]), [[10, 11], [14]])
                    c.write_file_records([(1, 1, [0xAAAA, 0xBBBB])])
                    self.assertEqual(c.read_file_records([(1, 0, 3)]), [[10, 0xAAAA, 0xBBBB]])
                    with self.assertRaises(ModbusException) as cm:
                        c.read_file_records([(9, 0, 1)])
                    self.assertEqual(cm.exception.code, 2)
                    self.assertEqual(c.read_fifo(100), [0x1234, 0x5678, 42])
                    with self.assertRaises(ModbusException):
                        c.read_fifo(101)
                    read = bytes([0, 0, 5, 0x20, 0, 0, 0, 0, 0, 4])
                    reply = c.canopen_request(read)
                    self.assertEqual(reply[0], 0)
                    self.assertEqual(reply[-4:], b"\xDE\xAD\xBE\xEF")
                    with self.assertRaises(ModbusException) as cm:
                        c.canopen_request(bytes([0, 0, 6, 0x20, 0, 0, 0, 0, 0, 4]))
                    self.assertEqual(cm.exception.code, 255)
                    self.assertEqual(cm.exception.data[0], 0xFF)
                    self.assertEqual(cm.exception.data[3:5], b"\x0D\xCE")
                    ident = c.read_device_identification(4, 0)
                    self.assertEqual(ident.conformity, 0x81)
                    self.assertEqual(ident.objects, {0: b"mogbus"})
                    self.assertEqual(set(c.read_device_id(read_code=3)) >= {0, 1, 2}, True)
                    with self.assertRaises(MogbusError):
                        c.read_device_id(read_code=7)

    def test_host_names_and_open_errors(self):
        port = self.serve()
        with Client(host="localhost", port=port) as c:
            self.assertEqual(c.read_holding_registers(0, 1), [100])
        c = Client(host="no-such-host.invalid", port=port)
        with self.assertRaisesRegex(MogbusError, "no-such-host.invalid"):
            c.connect()
        c.close()
        with self.assertRaises(MogbusError):
            Client(host="bad host!")
        with self.assertRaises(MogbusError):
            Client(mode="udp", tls=True)

    def test_byte_timeout(self):
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(2)
        self.addCleanup(listener.close)

        def serve(stall):
            conn, _ = listener.accept()
            with conn:
                head = conn.recv(12)
                reply = struct.pack(">HHHBBBH", struct.unpack(">H", head[:2])[0], 0, 5, head[6], 3, 2, 42)
                conn.sendall(reply[:8])
                time.sleep(stall)
                try:
                    conn.sendall(reply[8:])
                    conn.recv(1)
                except OSError:
                    pass

        port = listener.getsockname()[1]
        t = threading.Thread(target=serve, args=(0.6,))
        t.start()
        with Client(port=port, timeout_ms=3000, byte_timeout_ms=100) as c:
            started = time.monotonic()
            with self.assertRaisesRegex(MogbusError, "stalled") as cm:
                c.read_holding_registers(0, 1)
            self.assertEqual(cm.exception.status, Status.TIMEOUT)
            self.assertLess(time.monotonic() - started, 0.5)
        t.join()
        t = threading.Thread(target=serve, args=(0.2,))
        t.start()
        with Client(port=port, timeout_ms=3000) as c:
            self.assertEqual(c.read_holding_registers(0, 1), [42])
        t.join()

    def test_tls(self):
        try:
            Client(tls=True, tls_ca_file="tests/tls/ca.pem")
        except MogbusError as e:
            if e.status == Status.UNSUPPORTED:
                self.skipTest("the library is built without TLS")
            raise
        ctx = mogbus.Context()
        self.addCleanup(ctx.close)
        ctx.add_device(1).add_point("holding", 0, "u16", 77)
        port = free_port()
        server = ctx.serve(port=port, tls_enabled=True, tls_certificate="tests/tls/server.pem",
                           tls_private_key="tests/tls/server.key", tls_ca_certificate="tests/tls/ca.pem",
                           tls_require_client_certificate=True).start()
        self.addCleanup(server.close)
        with Client(port=port, tls=True, tls_ca_file="tests/tls/ca.pem", tls_cert_file="tests/tls/client.pem",
                    tls_key_file="tests/tls/client.key") as c:
            self.assertEqual(c.read_holding_registers(0, 1), [77])
        with Client(port=port, tls=True, tls_ca_file="tests/tls/ca.pem", tls_cert_file="tests/tls/client.pem",
                    tls_key_file="tests/tls/client.key", tls_server_name="wrong.example") as c:
            with self.assertRaisesRegex(MogbusError, "hostname mismatch"):
                c.read_holding_registers(0, 1)
        with Client(port=port, tls=True, tls_ca_file="tests/tls/ca.pem") as c:
            with self.assertRaises(MogbusError) as cm:
                c.read_holding_registers(0, 1)
            self.assertEqual(cm.exception.status, Status.IO)

    def test_exception_timeout_and_arguments(self):
        port = self.serve()
        with Client(port=port, timeout_ms=150, retries=1) as c:
            with self.assertRaises(ModbusException) as cm:
                c.read_holding_registers(5000, 1)
            self.assertEqual(cm.exception.code, 2)
            self.assertEqual(cm.exception.status, Status.EXCEPTION)
            self.assertIsInstance(cm.exception, MogbusError)
            with self.assertRaises(ModbusException) as cm:
                c.read_holding_registers(0, 1, unit=9)
            self.assertEqual(cm.exception.code, 11)
            with self.assertRaises(ModbusException) as cm:
                c.request(0x43)
            self.assertEqual(cm.exception.code, 5)
            with self.assertRaises(MogbusError) as cm:
                c.request(0x42)
            self.assertEqual(cm.exception.status, Status.TIMEOUT)
            self.assertEqual(c.read_holding_registers(1, 1), [101])  # reconnected after the timeout
            with self.assertRaises(MogbusError):
                c.read_holding_registers(0, 0)
            with self.assertRaises(MogbusError):
                c.read_holding_registers(70000, 1)
            with self.assertRaises(MogbusError):
                c.write_register(0, 70000)

    def test_open_errors_and_closed_client(self):
        with self.assertRaisesRegex(MogbusError, "not an ip"):
            Client(host="not an ip")
        with self.assertRaises(MogbusError):
            Client(mode="rtu-pty")
        c = Client(port=free_port())
        with self.assertRaises(MogbusError) as cm:
            c.connect()
        self.assertEqual(cm.exception.status, Status.IO)
        c.close()
        c.close()
        with self.assertRaisesRegex(MogbusError, "closed"):
            c.read_coils(0, 1)

    def test_default_unit(self):
        port = self.serve()
        with Client(port=port, unit=9) as c:
            with self.assertRaises(ModbusException):
                c.read_holding_registers(0, 1)
            self.assertEqual(c.read_holding_registers(0, 1, unit=1), [100])


if __name__ == "__main__":
    unittest.main()
