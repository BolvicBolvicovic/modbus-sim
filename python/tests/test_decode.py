"""Tests for mogbus.decode_frame (the frame decoder)."""
import unittest

import mogbus


class DecodeFrameTests(unittest.TestCase):
    def test_text_of_an_rtu_request(self):
        text = mogbus.decode_frame(bytes.fromhex("010300000001840A"))
        self.assertIn("framing: rtu", text)
        self.assertIn("function: 3 (0x03) Read Holding Registers", text)
        self.assertIn("direction: request", text)
        self.assertIn("crc_ok: true", text)
        self.assertIn("ok: true", text)

    def test_dict_has_the_same_fields(self):
        frame = mogbus.decode_frame("00 01 00 00 00 06 01 03 00 00 00 02", as_dict=True)
        self.assertEqual(frame["framing"], "mbap")
        self.assertEqual(frame["adu"], {"transaction": 1, "protocol": 0, "length": 6, "unit": 1})
        self.assertEqual(frame["pdu"]["function"], 3)
        self.assertEqual(frame["pdu"]["function_name"], "Read Holding Registers")
        self.assertEqual(frame["pdu"]["quantity"], 2)
        self.assertTrue(frame["ok"])

    def test_hex_spellings_and_ascii_text(self):
        for spelling in ("01 03 00 00 00 01 84 0A", "01:03:00:00:00:01:84:0A", "0x01,0x03,0x00,0x00,0x00,0x01,0x84,0x0A",
                         "010300000001840A", bytearray.fromhex("010300000001840A")):
            self.assertTrue(mogbus.decode_frame(spelling, as_dict=True)["adu"]["crc_ok"])
        for text in (":010300000001FB", ":010300000001FB\r\n"):
            frame = mogbus.decode_frame(text, as_dict=True)
            self.assertEqual(frame["framing"], "ascii")
            self.assertTrue(frame["adu"]["lrc_ok"])

    def test_forced_framing_and_direction(self):
        frame = mogbus.decode_frame("01 03 02 00 0A 38 43", framing=mogbus.Framing.RTU, direction="response",
                                    as_dict=True)
        self.assertEqual(frame["pdu"]["direction"], "response")
        self.assertNotIn("direction_reason", frame["pdu"])
        self.assertEqual(frame["pdu"]["values"], [10])
        auto = mogbus.decode_frame("01 03 02 00 0A 38 43", as_dict=True)
        self.assertEqual(auto["pdu"]["direction"], "response")
        self.assertIn("direction_reason", auto["pdu"])

    def test_a_damaged_frame_is_decoded_not_raised(self):
        frame = mogbus.decode_frame("01 03 00 00 00 01 84 0B", as_dict=True)
        self.assertFalse(frame["ok"])
        self.assertFalse(frame["adu"]["crc_ok"])
        self.assertTrue(frame["errors"][0].startswith("CRC mismatch"))
        self.assertEqual(frame["pdu"]["quantity"], 1)
        self.assertIn("ok: false", mogbus.decode_frame("01 03 00 00 00 01 84 0B"))

    def test_exception_and_ambiguity(self):
        frame = mogbus.decode_frame("01 83 02 C0 F1", as_dict=True)
        self.assertEqual(frame["pdu"]["exception_name"], "Illegal Data Address")
        data = bytearray.fromhex("000100000006010300000000")
        crc = 0xFFFF
        for byte in data[:10]:
            crc ^= byte
            for _ in range(8):
                crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
        data[10], data[11] = crc & 0xFF, crc >> 8
        both = mogbus.decode_frame(bytes(data), as_dict=True)
        self.assertTrue(both["ambiguous"])
        self.assertEqual([c["framing"] for c in both["candidates"]], ["mbap", "rtu"])

    def test_bad_arguments_raise(self):
        with self.assertRaises(mogbus.MogbusError):
            mogbus.decode_frame("zz")
        with self.assertRaises(mogbus.MogbusError):
            mogbus.decode_frame(b"\x01" * 5000)
        with self.assertRaises(mogbus.MogbusError):
            mogbus.decode_frame(b"\x01\x03", framing="udp")
        with self.assertRaises(mogbus.MogbusError):
            mogbus.decode_frame(b"\x01\x03", direction="sideways")


if __name__ == "__main__":
    unittest.main()
