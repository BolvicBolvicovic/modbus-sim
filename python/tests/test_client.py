"""mogbus.Client against a server of the same process, started on a background thread."""
import socket
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
        with self.assertRaisesRegex(MogbusError, "not-an-ip"):
            Client(host="not-an-ip")
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
