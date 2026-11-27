"""A tiny Modbus TCP client for the pump station tutorial (standard library only).

It stands in for "your own client code": one connection, one request at a time, the function
codes the tutorial uses. An exception reply raises ModbusException with the Modbus exception code.
"""
import socket
import struct


class ModbusException(Exception):
    """The device answered with a Modbus exception."""

    def __init__(self, function, code):
        super().__init__(f"function 0x{function:02X} failed with exception {code:02d}")
        self.function = function
        self.code = code


class Client:
    def __init__(self, port, host="127.0.0.1", unit=1, timeout=2.0):
        self.unit = unit
        self.tid = 0
        self.sock = socket.create_connection((host, port), timeout=timeout)

    def close(self):
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _recv(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ConnectionError("the server closed the connection")
            data += chunk
        return data

    def call(self, pdu):
        """Sends one PDU inside an MBAP frame and returns the reply PDU."""
        self.tid = (self.tid + 1) & 0xFFFF
        self.sock.sendall(struct.pack(">HHHB", self.tid, 0, len(pdu) + 1, self.unit) + pdu)
        header = self._recv(7)
        reply = self._recv(struct.unpack(">H", header[4:6])[0] - 1)
        if reply[0] & 0x80:
            raise ModbusException(reply[0] & 0x7F, reply[1])
        return reply

    # Reads: return raw register values, or bits as 0/1.
    def registers(self, function, address, count):
        reply = self.call(struct.pack(">BHH", function, address, count))
        return list(struct.unpack(">%dH" % (reply[1] // 2), reply[2:]))

    def bits(self, function, address, count):
        reply = self.call(struct.pack(">BHH", function, address, count))
        return [(reply[2 + i // 8] >> (i % 8)) & 1 for i in range(count)]

    def holding(self, address, count=1):
        return self.registers(3, address, count)

    def input(self, address, count=1):
        return self.registers(4, address, count)

    def coils(self, address, count=1):
        return self.bits(1, address, count)

    def discrete(self, address, count=1):
        return self.bits(2, address, count)

    # Writes.
    def write_coil(self, address, on):
        self.call(struct.pack(">BHH", 5, address, 0xFF00 if on else 0))

    def write_register(self, address, value):
        self.call(struct.pack(">BHH", 6, address, value))

    def write_registers(self, address, values):
        self.call(struct.pack(">BHHB%dH" % len(values), 16, address, len(values), 2 * len(values), *values))

    # Decoded helpers.
    def read_f32(self, function, address, low_first=False):
        hi, lo = self.registers(function, address, 2)
        if low_first:
            hi, lo = lo, hi
        return struct.unpack(">f", struct.pack(">HH", hi, lo))[0]

    def write_f32(self, address, value):
        hi, lo = struct.unpack(">HH", struct.pack(">f", value))
        self.write_registers(address, [hi, lo])

    def read_string(self, address, length):
        raw = struct.pack(">%dH" % length, *self.holding(address, length))
        return raw.split(b"\0")[0].decode("latin-1")

    # FC 0x14 (one register per record) and FC 0x18.
    def file_records(self, file, record, count):
        pdu = struct.pack(">BBBHHH", 0x14, 7, 6, file, record, count)
        reply = self.call(pdu)
        return list(struct.unpack(">%dH" % count, reply[4:4 + 2 * count]))

    def fifo(self, address):
        reply = self.call(struct.pack(">BH", 0x18, address))
        count = struct.unpack(">H", reply[3:5])[0]
        return list(struct.unpack(">%dH" % count, reply[5:5 + 2 * count]))
