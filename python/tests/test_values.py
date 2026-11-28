"""Tests for mogbus.registers_to_value, value_to_registers and the text helpers."""
import math
import unittest

import mogbus

# The four layouts of 0x0A0B0C0D and of float 1.0 (0x3F800000): name -> (word order, byte order, bits, one).
LAYOUTS = {
    "ABCD": ("high-first", "big", [0x0A0B, 0x0C0D], [0x3F80, 0x0000]),
    "CDAB": ("low-first", "big", [0x0C0D, 0x0A0B], [0x0000, 0x3F80]),
    "BADC": ("high-first", "little", [0x0B0A, 0x0D0C], [0x803F, 0x0000]),
    "DCBA": ("low-first", "little", [0x0D0C, 0x0B0A], [0x0000, 0x803F]),
}


class ValueHelperTests(unittest.TestCase):
    def test_the_four_layouts_of_32_bit_values(self):
        for name, (word, byte, bits, one) in LAYOUTS.items():
            with self.subTest(layout=name):
                self.assertEqual(mogbus.registers_to_value(bits, "u32", word, byte), 0x0A0B0C0D)
                self.assertEqual(mogbus.value_to_registers(0x0A0B0C0D, "u32", word, byte), bits)
                self.assertEqual(mogbus.registers_to_value(one, "f32", word, byte), 1.0)
                self.assertEqual(mogbus.value_to_registers(1.0, "f32", word, byte), one)

    def test_every_type_round_trips_in_every_order(self):
        cases = [("u16", 65535), ("s16", -2), ("u32", 4000000000), ("s32", -123456), ("f32", -0.5),
                 ("u64", 2 ** 64 - 1), ("s64", -(2 ** 63)), ("u64", 9007199254740993), ("f64", 3.141592653589793)]
        for type_, value in cases:
            for word in mogbus.WordOrder:
                for byte in mogbus.ByteOrder:
                    regs = mogbus.value_to_registers(value, type_, word, byte)
                    self.assertEqual(mogbus.registers_to_value(regs, type_, word, byte), value, (type_, word, byte))

    def test_result_types_and_widths(self):
        self.assertIsInstance(mogbus.registers_to_value([5], "u16"), int)
        self.assertIsInstance(mogbus.registers_to_value([0x3F80, 0], "f32"), float)
        self.assertEqual(len(mogbus.value_to_registers(1, mogbus.ValueType.F64)), 4)
        self.assertEqual(len(mogbus.value_to_registers(1, "s16")), 1)
        self.assertEqual(mogbus.registers_to_value([0xFFFE], "s16"), -2)
        self.assertEqual(mogbus.registers_to_value([0x0001, 0x0002, 0x0003, 0x0004], "u32"), 0x00010002)

    def test_clamping_and_non_finite_floats(self):
        self.assertEqual(mogbus.value_to_registers(70000, "u16"), [0xFFFF])
        self.assertEqual(mogbus.value_to_registers(2.6, "u16"), [3])
        self.assertEqual(mogbus.value_to_registers(float("nan"), "s32"), [0, 0])
        self.assertTrue(math.isnan(mogbus.registers_to_value(mogbus.value_to_registers(float("nan"), "f64"), "f64")))
        self.assertEqual(mogbus.registers_to_value(mogbus.value_to_registers(math.inf, "f32"), "f32"), math.inf)

    def test_errors(self):
        for bad in ("bool", "string", "nope"):
            with self.assertRaises(mogbus.MogbusError):
                mogbus.registers_to_value([1, 2], bad)
            with self.assertRaises(mogbus.MogbusError):
                mogbus.value_to_registers(1, bad)
        with self.assertRaises(mogbus.MogbusError):
            mogbus.registers_to_value([1], "u32")
        with self.assertRaises(mogbus.MogbusError):
            mogbus.registers_to_value([70000], "u16")
        with self.assertRaises(mogbus.MogbusError):
            mogbus.registers_to_value([1], "u16", word_order="sideways")

    def test_text(self):
        self.assertEqual(mogbus.text_to_registers("AB", 3), [0x4142, 0, 0])
        self.assertEqual(mogbus.text_to_registers("pump1", 3, "little"), [0x7570, 0x706D, 0x0031])
        self.assertEqual(mogbus.registers_to_text([0x7570, 0x706D, 0x0031], "little"), "pump1")
        self.assertEqual(mogbus.registers_to_text(mogbus.text_to_registers("abcdef", 3)), "abcdef")
        self.assertEqual(mogbus.registers_to_text([]), "")
        with self.assertRaises(mogbus.MogbusError):
            mogbus.text_to_registers("abcdefg", 3)
        with self.assertRaises(mogbus.MogbusError):
            mogbus.text_to_registers("café", 4)

    def test_decodes_what_a_point_serves(self):
        ctx = mogbus.Context()
        try:
            dev = ctx.add_device(1, "values")
            dev.add_point("holding", 0, "f32", 12.5, word_order="low-first", byte_order="little")
            dev.add_point("holding", 2, "u64", 2 ** 63)
            dev.add_point("holding", 6, "string", length=3, text="abcde", byte_order="little")
            self.assertEqual(mogbus.registers_to_value(dev.read("holding", 0, 2), "f32", "low-first", "little"), 12.5)
            self.assertEqual(mogbus.registers_to_value(dev.read("holding", 2, 4), "u64"), 2 ** 63)
            self.assertEqual(mogbus.registers_to_text(dev.read("holding", 6, 3), "little"), "abcde")
        finally:
            ctx.close()


if __name__ == "__main__":
    unittest.main()
