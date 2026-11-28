"""Tests for tools/mogbus_import.py (standard library only; no libmogbus needed)."""
import contextlib
import importlib.util
import io
import json
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("mogbus_import", ROOT / "tools" / "mogbus_import.py")
imp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(imp)


def mogbus_bin():
    path = os.environ.get("MOGBUS_BIN") or str(ROOT / ("mogbus.exe" if os.name == "nt" else "mogbus"))
    return path if os.path.exists(path) else None


class ImportCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def run_tool(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = imp.main([str(a) for a in argv])
        return code, out.getvalue(), err.getvalue()

    def write(self, name, text):
        path = Path(self.tmp.name) / name
        path.write_text(text)
        return path

    def csv(self, text, *opts, name="map.csv"):
        return self.run_tool("csv", self.write(name, text), *opts)

    def pm(self, doc, *opts):
        return self.run_tool("pymodbus", self.write("setup.json", json.dumps(doc)), *opts)

    def points(self, out):
        """Non-comment lines of the device body, whitespace collapsed."""
        lines = [" ".join(ln.split()) for ln in out.splitlines()
                 if ln.strip().startswith("(") and not ln.strip().startswith(("(device", "(name", "(unit-id", "(server", "(any-unit"))]
        # the last point carries the closing parenthesis of the device
        while lines and lines[-1].count(")") > lines[-1].count("("):
            lines[-1] = lines[-1][:-1]
        return lines


class CsvDetection(ImportCase):
    BODY = [["0", "Voltage", "float32", "230.5"], ["2", "Count", "u16", "7"]]

    def table(self, delim, title=True):
        rows = ([["Some vendor title, rev 3"], [""]] if title else [])
        rows += [["Address", "Name", "Type", "Default"]] + self.BODY
        return "\n".join(delim.join(r) if len(r) > 1 else r[0] for r in rows) + "\n"

    def test_delimiters_and_title_lines(self):
        for delim in (",", ";", "\t"):
            with self.subTest(delim=repr(delim)):
                code, out, _ = self.csv(self.table(delim))
                self.assertEqual(code, 0)
                pts = self.points(out)
                self.assertIn("(holding 0 (type float32) (static 230.5) (note \"Voltage\"))", pts)
                self.assertIn("(holding 2 (static 7) (note \"Count\"))", pts)

    def test_no_header_is_an_error(self):
        code, _, err = self.csv("a,b\n1,2\n")
        self.assertEqual(code, 1)
        self.assertIn("no header row", err)

    def test_address_aliases(self):
        for h in ("address", "Register", "REG", "Offset", "addr", "Modbus Address", "Address (dec)"):
            with self.subTest(h=h):
                code, out, _ = self.csv("%s,Name\n5,x\n" % h)
                self.assertEqual(code, 0)
                self.assertIn("(holding 5 ", out)

    def test_map_override(self):
        code, out, _ = self.csv("Slot,Label,Kind\n4,x,float32\n", "--map", "address=Slot,name=Label,type=Kind")
        self.assertEqual(code, 0)
        self.assertIn("(holding 4 (type float32) (note \"x\"))", self.points(out))
        code, _, err = self.csv("Slot,Label\n4,x\n", "--map", "address=Nope")
        self.assertEqual(code, 1)

    def test_name_description_unit_in_note(self):
        _, out, _ = self.csv("Address,Name,Description,Unit\n0,Temp,Room temperature,C\n")
        self.assertIn('(note "Temp - Room temperature (C)")', out)
        _, out, _ = self.csv("Address,Description,Units\n0,Room temperature,C\n")
        self.assertIn('(note "Room temperature (C)")', out)


class CsvTypesAndSpaces(ImportCase):
    def one(self, header, row, *opts):
        code, out, err = self.csv("%s\n%s\n" % (header, row), *opts)
        self.assertEqual(code, 0, err)
        return self.points(out)[0], out

    def test_space_values(self):
        cases = {"holding": "holding", "Holding Register": "holding", "4x": "holding", "3": "holding",
                 "6": "holding", "16": "holding", "input": "input", "Input Register": "input",
                 "3x": "input", "4": "input", "FC04": "input", "coil": "coil", "Coils": "coil",
                 "0x": "coil", "1": "coil", "5": "coil", "15": "coil", "discrete": "discrete",
                 "Discrete Input": "discrete", "1x": "discrete", "2": "discrete",
                 "0x03": "holding"}
        for text, table in cases.items():
            with self.subTest(text=text):
                p, _ = self.one("Address,Table", "7,%s" % text)
                self.assertTrue(p.startswith("(%s 7" % table), p)

    def test_space_header_aliases(self):
        for h in ("Table", "space", "Register Type", "Type of register", "Function Code", "FC",
                  "Object Type"):
            with self.subTest(h=h):
                p, _ = self.one("Address,%s" % h, "7,input")
                self.assertTrue(p.startswith("(input 7"), p)

    def test_modicon_decides_without_space_column(self):
        _, out, _ = self.csv("Address\n40001\n40010\n30001\n00001\n10003\n")
        pts = self.points(out)
        self.assertEqual([p.split(" ")[0:2] for p in pts],
                         [["(coil", "0)"], ["(discrete", "2)"], ["(input", "0)"],
                          ["(holding", "0)"], ["(holding", "9)"]])
        _, out, _ = self.csv("Address\n400001\n465536\n")
        self.assertEqual([p.split(" ")[0:2] for p in self.points(out)], [["(holding", "0)"], ["(holding", "65535)"]])

    def test_default_space_option(self):
        _, out, _ = self.csv("Address\n3\n", "--space", "input", "--base", "0")
        self.assertIn("(input 3)", out)

    def test_bool_without_space_column(self):
        p, _ = self.one("Address,Type,Access", "3,bool,RO")
        self.assertTrue(p.startswith("(discrete 3"), p)
        p, _ = self.one("Address,Type,Access", "3,bool,RW")
        self.assertTrue(p.startswith("(coil 3"), p)

    def test_bool_in_register_table_warns(self):
        code, out, err = self.csv("Address,Table,Type\n3,holding,bool\n")
        self.assertEqual(code, 0)
        self.assertIn("(holding 3", out)
        self.assertIn("stored as u16", err)

    def test_data_types(self):
        cases = {"u16": "u16", "uint16": "u16", "WORD": "u16", "unsigned": "u16",
                 "Unsigned 16-bit": "u16", "s16": "s16", "INT16": "s16", "short": "s16",
                 "signed": "s16", "INT": "s16", "u32": "u32", "UINT32": "u32", "UDINT": "u32",
                 "DWORD": "u32", "unsigned long": "u32", "s32": "s32", "int32": "s32",
                 "DINT": "s32", "long": "s32", "float": "float32", "float32": "float32",
                 "REAL": "float32", "IEEE754": "float32", "32-bit float": "float32", "u64": "u64",
                 "uint64": "u64", "ULINT": "u64", "s64": "s64", "INT64": "s64", "LINT": "s64",
                 "double": "float64", "float64": "float64", "LREAL": "float64"}
        for text, t in cases.items():
            with self.subTest(text=text):
                p, _ = self.one("Address,Data Type", "10,%s" % text)
                want = "(holding 10" if t == "u16" else "(holding 10 (type %s)" % t
                self.assertTrue(p.startswith(want), p)
        for h in ("type", "Data Type", "format", "datatype"):
            with self.subTest(h=h):
                p, _ = self.one("Address,%s" % h, "1,s32")
                self.assertIn("(type s32)", p)

    def test_unknown_type_skips_row(self):
        code, out, err = self.csv("Address,Type\n1,bcd\n2,u16\n")
        self.assertEqual(code, 0)
        self.assertEqual(len(self.points(out)), 1)
        self.assertIn("unknown data type", err)

    def test_default_type_option(self):
        p, _ = self.one("Address", "1", "--type", "float32")
        self.assertIn("(type float32)", p)

    def test_bool_default_values(self):
        for text, want in (("on", "on"), ("1", "on"), ("true", "on"), ("off", "off"), ("0", "off")):
            p, _ = self.one("Address,Table,Default", "1,coil,%s" % text)
            self.assertIn("(static %s)" % want, p)

    def test_u64_exact_and_hex(self):
        p, _ = self.one("Address,Type,Default", "0,u64,18446744073709551615")
        self.assertIn("(static #xFFFFFFFFFFFFFFFF)", p)
        p, _ = self.one("Address,Type,Default", "0,s64,-9223372036854775808")
        self.assertIn("(static -9223372036854775808)", p)

    def test_out_of_range_default_is_clamped_with_warning(self):
        code, out, err = self.csv("Address,Type,Default\n0,u16,70000\n")
        self.assertIn("(static 65535)", out)
        self.assertIn("outside u16", err)


class CsvByteLayouts(ImportCase):
    def layout(self, text, header="Endianness", typ="float32"):
        code, out, err = self.csv("Address,Type,%s\n0,%s,%s\n" % (header, typ, text))
        self.assertEqual(code, 0, err)
        return self.points(out)[0]

    def test_four_layouts(self):
        for text in ("ABCD", "abcd", "A B C D", "1234", "big endian", "Big-Endian"):
            self.assertNotIn("word-order", self.layout(text))
            self.assertNotIn("byte-order", self.layout(text))
        for text in ("CDAB", "3412", "word swap", "Word Swapped", "mid-little endian"):
            p = self.layout(text)
            self.assertIn("(word-order low-first)", p, text)
            self.assertNotIn("byte-order", p)
        for text in ("BADC", "2143", "byte swap", "mid-big endian"):
            p = self.layout(text)
            self.assertIn("(byte-order little)", p, text)
            self.assertNotIn("word-order", p)
        for text in ("DCBA", "4321", "little endian", "word and byte swap", "Byte and Word Swap"):
            p = self.layout(text)
            self.assertIn("(word-order low-first)", p, text)
            self.assertIn("(byte-order little)", p, text)

    def test_header_aliases(self):
        for h in ("Endianness", "endian", "Swap", "Byte Layout", "Word Order", "Byte Order"):
            with self.subTest(h=h):
                self.assertIn("(word-order low-first)", self.layout("CDAB", header=h))

    def test_separate_word_and_byte_columns(self):
        code, out, _ = self.csv("Address,Type,Word Order,Byte Order\n0,u32,low first,little\n")
        p = self.points(out)[0]
        self.assertIn("(word-order low-first)", p)
        self.assertIn("(byte-order little)", p)

    def test_layout_in_type_cell_and_default_option(self):
        _, out, _ = self.csv("Address,Type\n0,float32 CDAB\n")
        self.assertIn("(word-order low-first)", out)
        _, out, _ = self.csv("Address,Type\n0,u32\n", "--layout", "DCBA")
        self.assertIn("(word-order low-first) (byte-order little)", " ".join(out.split()))

    def test_16_bit_has_no_word_order(self):
        p = self.layout("DCBA", typ="u16")
        self.assertNotIn("word-order", p)
        self.assertIn("(byte-order little)", p)

    def test_bare_swap_warns(self):
        code, out, err = self.csv("Address,Type,Swap\n0,u32,yes\n")
        self.assertIn("(word-order low-first)", out)
        self.assertIn("assuming word swap", err)

    def test_unknown_layout_warns(self):
        _, _, err = self.csv("Address,Type,Byte Order\n0,u32,sideways\n")
        self.assertIn("unknown byte layout", err)


class CsvStrings(ImportCase):
    def string(self, header, row, *opts):
        code, out, err = self.csv("%s\n%s\n" % (header, row), *opts)
        return code, " ".join(self.points(out)[0].split()) if code == 0 else err, err

    def test_length_in_type_cell_is_bytes(self):
        for cell in ("string(10)", "STRING[10]", "String 10", "ascii(10)", "char[10]"):
            _, p, _ = self.string("Address,Type", "0,%s" % cell)
            self.assertIn("(length 5)", p, cell)
        _, p, _ = self.string("Address,Type", "0,string(7)")
        self.assertIn("(length 4)", p)
        _, p, _ = self.string("Address,Type", "0,string(10 regs)")
        self.assertIn("(length 10)", p)

    def test_length_column_is_registers_unless_header_says_bytes(self):
        for h in ("Length", "Size", "Registers", "Count", "Number of registers"):
            _, p, _ = self.string("Address,Type,%s" % h, "0,string,6")
            self.assertIn("(length 6)", p, h)
        _, p, _ = self.string("Address,Type,Length (bytes)", "0,string,12")
        self.assertIn("(length 6)", p)

    def test_default_text_and_truncation(self):
        _, p, _ = self.string("Address,Type,Default", "0,string(8),Hello")
        self.assertIn('(static "Hello")', p)
        _, p, err = self.string("Address,Type,Default", "0,string(4),Hello World")
        self.assertIn('(static "Hell")', p)
        self.assertIn("truncated", err)

    def test_missing_length_is_an_error(self):
        code, err, _ = self.string("Address,Type", "0,string")
        self.assertEqual(code, 1)
        self.assertIn("needs a length", err)

    def test_string_width_counts_for_overlap(self):
        code, _, err = self.csv("Address,Type\n0,string(8)\n3,u16\n")
        self.assertEqual(code, 1)
        self.assertIn("overlaps", err)


class CsvAddressing(ImportCase):
    def test_base_auto_detects_one_based(self):
        code, out, err = self.csv("Address,Name\n1,a\n2,b\n")
        self.assertIn("(holding 0 ", out)
        self.assertIn("(holding 1 ", out)
        self.assertIn("1-based", err)

    def test_base_auto_keeps_zero_based(self):
        _, out, err = self.csv("Address,Name\n0,a\n1,b\n")
        self.assertIn("(holding 0 ", out)
        self.assertNotIn("1-based", err)

    def test_explicit_base(self):
        _, out, _ = self.csv("Address\n1\n2\n", "--base", "0")
        self.assertIn("(holding 1)", out)
        _, out, _ = self.csv("Address\n5\n", "--base", "1")
        self.assertIn("(holding 4)", out)

    def test_base_below_zero_is_error(self):
        code, _, err = self.csv("Address\n0\n", "--base", "1")
        self.assertEqual(code, 1)
        self.assertIn("below the base", err)

    def test_modicon_base_is_one_and_ignores_plain_minimum(self):
        _, out, _ = self.csv("Address\n40001\n")
        self.assertIn("(holding 0)", out)

    def test_mixed_column_is_plain(self):
        code, out, err = self.csv("Address,Table\n100,holding\n40001,holding\n")
        self.assertIn("(holding 40001)", out)
        self.assertIn("look like Modicon", err)

    def test_modicon_off(self):
        _, out, _ = self.csv("Address,Table\n40001,holding\n", "--modicon", "no", "--base", "0")
        self.assertIn("(holding 40001)", out)

    def test_hex_addresses_and_bit_addresses(self):
        _, out, _ = self.csv("Address,Table\n0x10,holding\n10h,input\n", "--base", "0")
        self.assertIn("(holding 16)", out)
        self.assertIn("(input 16)", out)
        code, out, err = self.csv("Address\n40001.3\n0\n", "--base", "0")
        self.assertIn("bit address", err)

    def test_table_column_beats_modicon_prefix_with_warning(self):
        _, out, err = self.csv("Address,Table\n40001,input\n")
        self.assertIn("(input 0)", out)
        self.assertIn("using the column", err)

    def test_overlap_is_an_error(self):
        code, out, err = self.csv("Address,Type\n0,u32\n1,u16\n")
        self.assertEqual(code, 1)
        self.assertEqual(out, "")
        self.assertIn("overlaps", err)

    def test_duplicate_address_is_an_overlap(self):
        code, _, _ = self.csv("Address\n3\n3\n", "--base", "0")
        self.assertEqual(code, 1)

    def test_gap_is_fine_and_silent(self):
        code, out, err = self.csv("Address,Type\n0,u32\n10,u16\n")
        self.assertEqual(code, 0)
        self.assertEqual(err, "")

    def test_same_address_in_two_tables_is_fine(self):
        code, _, _ = self.csv("Address,Table\n0,holding\n0,input\n0,coil\n0,discrete\n")
        self.assertEqual(code, 0)

    def test_runs_past_table_end(self):
        code, _, err = self.csv("Address,Type\n65535,u32\n", "--base", "0")
        self.assertEqual(code, 1)
        self.assertIn("65535", err)


class CsvWarningsAndOptions(ImportCase):
    def test_access_mismatches(self):
        _, _, err = self.csv("Address,Table,Access\n0,input,RW\n1,discrete,W\n")
        self.assertIn("writable, but the input table is read-only", err)
        self.assertIn("writable, but the discrete table is read-only", err)

    def test_read_only_holding_and_coil_rows(self):
        code, out, err = self.csv("Address,Table,Access\n0,holding,R\n1,holding,RW\n2,coil,RO\n3,coil,\n")
        self.assertEqual(code, 0)
        self.assertEqual(err, "")
        pts = self.points(out)
        self.assertEqual([("access read-only" in p) for p in pts], [True, False, True, False])

    def test_access_matching_is_silent(self):
        for text in ("R", "RO", "Read only", "RW", "R/W", "read/write"):
            table = "input" if text.lower().startswith(("r", "read")) and "w" not in text.lower() else "holding"
            _, _, err = self.csv("Address,Table,Access\n0,%s,%s\n" % (table, text))
            self.assertEqual(err, "", text)

    def test_access_does_not_change_table(self):
        _, out, _ = self.csv("Address,Table,Access\n0,holding,R\n")
        self.assertIn("(holding 0", out)

    def test_scale_warns_and_goes_into_note(self):
        for h in ("Scale", "Multiplier", "Factor", "Gain"):
            code, out, err = self.csv("Address,Name,%s\n0,Volt,0.1\n" % h)
            self.assertIn("raw (scaled) value", err, h)
            self.assertIn("raw x 0.1", out, h)
        _, _, err = self.csv("Address,Scale\n0,1\n")
        self.assertEqual(err, "")

    def test_default_header_aliases(self):
        for h in ("Default", "Value", "Initial", "Initial Value", "Default Value"):
            _, out, _ = self.csv("Address,%s\n0,42\n" % h)
            self.assertIn("(static 42)", out, h)

    def test_warnings_are_in_the_output_as_comments(self):
        _, out, err = self.csv("Address,Scale\n0,0.1\n")
        self.assertIn("; Warnings:", out)
        self.assertTrue(any(ln.startswith(";") and "scale 0.1" in ln for ln in out.splitlines()))
        self.assertIn("warning: line 2", err)

    def test_strict(self):
        code, out, err = self.csv("Address,Scale\n0,0.1\n", "--strict")
        self.assertEqual(code, 1)
        self.assertEqual(out, "")
        self.assertIn("--strict", err)
        code, _, _ = self.csv("Address\n0\n", "--strict")
        self.assertEqual(code, 0)

    def test_device_and_server_options(self):
        _, out, _ = self.csv("Address\n0\n", "--unit-id", "7", "--name", "Pump",
                             "--server", "rtu-tcp", "--port", "1503", "--bind", "0.0.0.0")
        self.assertIn('(name "Pump")', out)
        self.assertIn("(unit-id 7)", out)
        self.assertIn('(server (mode rtu-tcp) (port 1503) (bind "0.0.0.0"))', out)
        _, out, _ = self.csv("Address\n0\n")
        self.assertNotIn("(server", out)
        self.assertIn('(name "map")', out)
        code, _, err = self.csv("Address\n0\n", "--server", "rtu-device")
        self.assertEqual(code, 1)
        self.assertIn("--path", err)

    def test_output_file_and_header_comment(self):
        out_path = Path(self.tmp.name) / "out.scm"
        code, out, _ = self.csv("Address\n0\n", "-o", out_path)
        self.assertEqual(code, 0)
        self.assertEqual(out, "")
        text = out_path.read_text()
        self.assertTrue(text.startswith("; Generated by tools/mogbus_import.py"))
        self.assertIn("Source: CSV file map.csv", text)

    def test_quotes_in_notes_are_escaped(self):
        _, out, _ = self.csv('Address,Name\n0,"Say ""hi"" \\ there"\n')
        self.assertIn('(note "Say \\"hi\\" \\\\ there")', out)

    def test_semicolon_decimal_comma_default(self):
        _, out, _ = self.csv("Address;Type;Default\n0;float32;230,5\n")
        self.assertIn("(static 230.5)", out)

    def test_latin1_file(self):
        path = Path(self.tmp.name) / "l1.csv"
        path.write_bytes("Address,Name\n0,Temp \xb0C\n".encode("latin-1"))
        code, out, _ = self.run_tool("csv", path)
        self.assertEqual(code, 0)
        self.assertIn("Temp °C", out)


def pm_doc(device=None, servers=None, **setup):
    base = {"co size": 64, "di size": 64, "hr size": 200, "ir size": 200, "shared blocks": True,
            "type exception": False,
            "defaults": {"value": {"bits": 0, "uint16": 0, "uint32": 0, "float32": 0.0, "string": " "},
                         "action": {"bits": None, "uint16": None, "uint32": None, "float32": None,
                                    "string": None}}}
    base.update(setup)
    dev = {"setup": base, "invalid": [], "write": [], "bits": [], "uint16": [], "uint32": [],
           "float32": [], "string": [], "repeat": []}
    dev.update(device or {})
    doc = {"device_list": {"dev": dev}}
    if servers:
        doc["server_list"] = servers
    return doc


class Pymodbus(ImportCase):
    def body(self, doc, *opts):
        code, out, err = self.pm(doc, *opts)
        self.assertEqual(code, 0, err)
        return self.points(out), out, err

    def test_types_and_values(self):
        pts, _, _ = self.body(pm_doc({
            "uint16": [{"addr": 3, "value": 17001}, {"addr": 4}],
            "uint32": [{"addr": [10, 11], "value": 617001}],
            "float32": [{"addr": [20, 21], "value": 404.17}],
            "string": [{"addr": [30, 34], "value": "A_B_C_D_E_"}]}))
        joined = "\n".join(pts)
        self.assertIn("(holding 3 (access read-only) (static 17001)", joined)
        self.assertIn("(holding 4 (access read-only) (static 0)", joined)
        self.assertIn("(holding 10 (type u32) (access read-only) (static 617001)", joined)
        self.assertIn("(holding 20 (type float32) (access read-only) (static 404.17)", joined)
        self.assertIn('(holding 30 (type string) (length 5) (padding space) (access read-only) (static "A_B_C_D_E_")', joined)

    def test_defaults_apply(self):
        pts, _, _ = self.body(pm_doc({"uint16": [7, [9, 10]], "float32": [[20, 21]]},
                                      defaults={"value": {"uint16": 5, "float32": 1.5}, "action": {}}))
        joined = "\n".join(pts)
        self.assertIn("(holding 7 (access read-only) (static 5)", joined)
        self.assertIn("(holding 9 (access read-only) (static 5) (count 2)", joined)
        self.assertIn("(holding 20 (type float32) (access read-only) (static 1.5)", joined)

    def test_shared_blocks_emit_both_tables(self):
        pts, out, _ = self.body(pm_doc({"uint16": [{"addr": 3, "value": 9}], "bits": [{"addr": 0, "value": 1}]}))
        self.assertTrue(any(p.startswith("(holding 3 ") for p in pts))
        self.assertTrue(any(p.startswith("(input 3 ") for p in pts))
        self.assertTrue(any(p.startswith("(coil 0 (access read-only) (static on)") for p in pts))
        self.assertTrue(any(p.startswith("(discrete 0 (static on)") for p in pts))
        self.assertIn("one register array for all tables", out)

    def test_separate_blocks_map_by_offset(self):
        doc = pm_doc({"bits": [0, 4], "uint16": [{"addr": 4 + 4 + 2, "value": 11},
                                                  {"addr": 4 + 4 + 8 + 3, "value": 22}]},
                     **{"shared blocks": False, "co size": 4, "di size": 4, "ir size": 8, "hr size": 8})
        pts, _, _ = self.body(doc)
        joined = "\n".join(pts)
        self.assertIn("(coil 0 (access read-only) (static off) (count 16)", joined)
        self.assertIn("(discrete 0 (static off) (count 16)", joined)
        self.assertIn("(input 2 (static 11)", joined)
        self.assertIn("(holding 3 (access read-only) (static 22)", joined)
        self.assertNotIn("(holding 2 ", joined)

    def test_bits_cell_expands_to_sixteen_coils(self):
        pts, _, _ = self.body(pm_doc({"bits": [{"addr": 1, "value": 0b101}]}))
        self.assertIn("(coil 16 (access read-only) (static on) (note \"pymodbus bits, bit 16\"))", pts)
        self.assertIn("(coil 17 (access read-only) (static off) (note \"pymodbus bits, bit 17\"))", pts)
        self.assertTrue(any(p.startswith("(coil 19 (access read-only) (static off) (count 13)") for p in pts))

    def test_write_list_makes_the_rest_read_only(self):
        pts, _, err = self.body(pm_doc({"uint16": [{"addr": 1, "value": 1}, {"addr": 2, "value": 2}],
                                        "bits": [{"addr": 0, "value": 1}],
                                        "write": [1, 2]}))
        self.assertEqual(err, "")
        joined = "\n".join(pts)
        self.assertIn("(holding 1 (static 1)", joined)
        self.assertIn("(holding 2 (static 2)", joined)
        self.assertIn("(input 1 (static 1)", joined)
        self.assertIn("(coil 0 (access read-only)", joined)
        self.assertNotIn("(discrete 0 (access", joined)
        pts, _, err = self.body(pm_doc({"uint16": [{"addr": 1, "value": 1}, {"addr": 2, "value": 2}],
                                        "write": [1]}))
        self.assertIn("(holding 1 (static 1)", "\n".join(pts))
        self.assertIn("(holding 2 (access read-only) (static 2)", "\n".join(pts))
        self.assertNotIn("(input 2 (access", "\n".join(pts))

    def test_write_of_undefined_cell_is_an_error(self):
        code, _, err = self.pm(pm_doc({"write": [5]}))
        self.assertEqual(code, 1)
        self.assertIn("not defined", err)

    def test_overlapping_entries_are_an_error(self):
        code, _, err = self.pm(pm_doc({"uint16": [3], "uint32": [[2, 3]]}))
        self.assertEqual(code, 1)
        self.assertIn("defined twice", err)

    def test_increment_becomes_ramp(self):
        pts, _, _ = self.body(pm_doc({"uint16": [
            {"addr": 5, "value": 50, "action": "increment", "parameters": {"minval": 45, "maxval": 155}},
            {"addr": 6, "value": 3, "action": "increment"}]}))
        joined = "\n".join(pts)
        self.assertIn("(holding 5 (access read-only) (ramp (from 45) (to 155) (step 1) (every-ms 1000) (mode wrap))", joined)
        self.assertIn("(holding 6 (access read-only) (ramp (from 3) (to 65535) (step 1) (every-ms 1000) (mode wrap))", joined)

    def test_increment_float_and_action_ms(self):
        pts, _, _ = self.body(pm_doc({"float32": [{"addr": [4, 5], "value": 1.5, "action": "increment"}]}),
                              "--action-ms", "250")
        self.assertIn("(ramp (from 1.5) (to 16777216) (step 1) (every-ms 250) (mode wrap))", "\n".join(pts))

    def test_random_becomes_noise_around_the_middle(self):
        pts, _, _ = self.body(pm_doc({"uint16": [
            {"addr": 5, "action": "random", "parameters": {"minval": 45, "maxval": 55}}],
            "float32": [{"addr": [10, 11], "action": "random", "parameters": {"minval": 19.0, "maxval": 22.0}}]}))
        joined = "\n".join(pts)
        self.assertIn("(holding 5 (access read-only) (static 50) (noise (amplitude 5) (every-ms 1000))", joined)
        self.assertIn("(holding 10 (type float32) (access read-only) (static 20.5) (noise (amplitude 1.5) (every-ms 1000))", joined)

    def test_uptime_becomes_holding_ramp(self):
        pts, _, _ = self.body(pm_doc({"uint32": [{"addr": [0, 1], "action": "uptime"}]}))
        self.assertIn("(ramp (from 1) (to 4294967295) (step 1) (every-ms 1000) (mode hold))", "\n".join(pts))

    def check_scm(self, name, out):
        if mogbus_bin():
            r = subprocess.run([mogbus_bin(), "--check", str(self.write(name, out))],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_custom_action_becomes_a_stub(self):
        pts, out, err = self.body(pm_doc({"uint16": [
            {"addr": 1, "value": 9, "action": "my_custom", "parameters": {"gain": 2, "on": True, "tag": "x"}},
            {"addr": 2, "value": 4, "action": "my_custom", "parameters": {"gain": 5, "on": True}}]}))
        self.assertIn('; TODO: port the pymodbus action "my_custom" (Python) to statements', out)
        self.assertIn("(define-action my-custom\n  (param gain 2)\n  (param on #t)\n  (set! self self))", out)
        self.assertLess(out.index("(define-action"), out.index("(device"))
        self.assertEqual(out.count("(define-action"), 1)
        self.assertIn('parameter "tag" not imported', out)
        joined = "\n".join(pts)
        self.assertIn("(holding 1 (access read-only) (static 9) (on-read (my-custom))", joined)
        self.assertIn("(holding 2 (access read-only) (static 4) (on-read (my-custom (gain 5)))", joined)
        self.assertIn("action 'my_custom' is a custom Python action; imported as the stub define-action my-custom to fill in", err)
        self.check_scm("custom.scm", out)

    def test_custom_action_on_a_range_is_split_and_on_wide_points(self):
        pts, out, _ = self.body(pm_doc({
            "uint16": [{"addr": [1, 3], "value": 1, "action": "tick"}],
            "float32": [{"addr": [10, 11], "value": 1.5, "action": "tick"}]}))
        joined = "\n".join(pts)
        self.assertNotIn("(count", joined)
        self.assertEqual(joined.count("(on-read (tick))"), 8)   # 3 u16 + 1 float32, in holding and input
        self.assertIn("(holding 10 (type float32)", joined)
        self.check_scm("range.scm", out)

    def test_custom_action_on_bits(self):
        pts, out, _ = self.body(pm_doc({"bits": [{"addr": 0, "value": 5, "action": "blink"}]}))
        self.assertEqual("\n".join(pts).count("(on-read (blink))"), 32)
        self.assertEqual(out.count("(define-action blink"), 1)
        self.check_scm("bits-custom.scm", out)

    def test_custom_actions_over_the_device_limit_fail(self):
        code, _, err = self.pm(pm_doc({"bits": [{"addr": [0, 8], "value": 5, "action": "blink"}]}))
        self.assertNotEqual(code, 0)
        self.assertIn("at most 256 point actions", err)

    def test_custom_action_names_are_valid_and_unique(self):
        _, out, _ = self.body(pm_doc({"uint16": [
            {"addr": 1, "action": "set!"}, {"addr": 2, "action": "Set_!"},
            {"addr": 3, "action": "1st action"}, {"addr": 4, "action": "x" * 80}]}))
        names = re.findall(r"\(define-action (\S+)", out)
        self.assertEqual(len(set(names)), 4, names)
        self.assertIn("set", names)
        self.assertIn("set-2", names)
        self.assertIn("action-1st-action", names)
        self.assertTrue(all(re.fullmatch(r"[a-z][a-z0-9-]{0,62}", n) for n in names), names)
        self.check_scm("names.scm", out)

    def test_custom_action_on_a_string_stays_static(self):
        pts, out, err = self.body(pm_doc({"string": [{"addr": [0, 1], "value": "abcd", "action": "my_custom"}]}))
        self.assertNotIn("define-action", out)
        self.assertIn("action 'my_custom' on a string is not supported", err)

    def test_reset_becomes_a_failing_read(self):
        doc = pm_doc({"uint16": [{"addr": 1, "value": 9, "action": "reset"}]})
        pts, out, err = self.body(doc)
        self.assertIn("(holding 1 (access read-only) (static 9) (degradation (fail-after-ms 0))",
                      "\n".join(pts))
        self.assertIn("(input 1 (static 9) (degradation (fail-after-ms 0))", "\n".join(pts))
        self.assertEqual(err.strip(), "")
        if mogbus_bin():
            r = subprocess.run([mogbus_bin(), "--check", str(self.write("reset.scm", out))],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_reset_on_bits_applies_to_every_bit_of_the_cell(self):
        doc = pm_doc({"bits": [{"addr": 0, "value": 5, "action": "reset"}]})
        pts, out, err = self.body(doc)
        joined = "\n".join(pts)
        self.assertIn("(coil 0 (access read-only) (static on) (degradation (fail-after-ms 0))", joined)
        self.assertIn("(discrete 3 (static off) (degradation (fail-after-ms 0)) (count 13)", joined)
        self.assertNotIn("not supported", err)
        if mogbus_bin():
            r = subprocess.run([mogbus_bin(), "--check", str(self.write("resetbits.scm", out))],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    @unittest.skipUnless(mogbus_bin(), "mogbus binary not built")
    def test_reset_read_answers_exception_04(self):
        import socket
        import time
        doc = pm_doc({"uint16": [{"addr": 1, "value": 9, "action": "reset"}, {"addr": 2, "value": 7}]})
        code, out, _ = self.pm(doc)
        self.assertEqual(code, 0)
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        out += "\n(server (mode tcp) (port %d))\n" % port
        proc = subprocess.Popen([mogbus_bin(), str(self.write("srv.scm", out))],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(proc.wait)
        self.addCleanup(proc.terminate)

        def read(addr):
            req = bytes([0, 1, 0, 0, 0, 6, 1, 3, 0, addr, 0, 1])
            for _ in range(50):
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=2) as c:
                        c.sendall(req)
                        return c.recv(64)
                except OSError:
                    time.sleep(0.1)
            self.fail("server did not start")

        self.assertEqual(read(1)[7:], bytes([0x83, 0x04]))
        self.assertEqual(read(2)[7:], bytes([0x03, 0x02, 0x00, 0x07]))

    def test_timestamp_becomes_seven_clock_points(self):
        doc = pm_doc({"uint16": [{"addr": [10, 16], "action": "timestamp"}], "write": [[10, 16]]})
        pts, out, err = self.body(doc)
        fields = ("year", "month", "day", "weekday", "hour", "minute", "second")
        for k, f in enumerate(fields):
            self.assertTrue(any(p.startswith("(holding %d (clock %s)" % (10 + k, f)) for p in pts), f)
            self.assertTrue(any(p.startswith("(input %d (clock %s)" % (10 + k, f)) for p in pts), f)
        self.assertIn("month as 0-11", err)
        self.assertEqual(err.count("month as 0-11"), 1)
        self.assertIn("pymodbus cells 11, 12, 13, 14, 15, 16: timestamp action inside", err)
        if mogbus_bin():
            path = self.write("ts.scm", out)
            r = subprocess.run([mogbus_bin(), "--check", path], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_timestamp_single_cell_overrides_and_missing_cells(self):
        doc = pm_doc({"uint16": [{"addr": 5, "action": "timestamp"}, {"addr": 7, "value": 3}]})
        pts, _, err = self.body(doc)
        self.assertIn("(holding 11 (access read-only) (clock second)", "\n".join(pts))
        self.assertIn("replace the declared cells 7", err)
        self.assertIn("cells 6, 8, 9, 10, 11 are not declared", err)

    def test_timestamp_on_a_string_warns(self):
        _, _, err = self.body(pm_doc({"string": [{"addr": [0, 3], "value": "abcd", "action": "timestamp"}]}))
        self.assertIn("action 'timestamp' on a string is not supported", err)

    def test_bits_action_warns(self):
        _, _, err = self.body(pm_doc({"bits": [{"addr": 0, "value": 1, "action": "increment"}]}))
        self.assertIn("not supported", err)

    def test_repeat_copies_cells(self):
        pts, _, _ = self.body(pm_doc({"uint16": [{"addr": 0, "value": 4}, {"addr": 1, "value": 5}],
                                      "repeat": [{"addr": [0, 1], "to": [10, 13]}]}))
        joined = "\n".join(pts)
        for addr, val in ((10, 4), (11, 5), (12, 4), (13, 5)):
            self.assertIn("(holding %d (access read-only) (static %d)" % (addr, val), joined)

    def test_devices(self):
        doc = pm_doc({"uint16": [{"addr": 1, "value": 11}]})
        doc["device_list"]["second"] = pm_doc({"uint16": [{"addr": 2, "value": 22}]})["device_list"]["dev"]
        code, out, err = self.pm(doc)
        self.assertEqual(code, 0)
        self.assertIn("(holding 1 (access read-only) (static 11)", out)
        self.assertIn("2 devices; using 'dev'", err)
        code, out, err = self.pm(doc, "--device", "second")
        self.assertIn("(holding 2 (access read-only) (static 22)", out)
        self.assertNotIn("2 devices", err)
        self.assertIn('(name "second")', out)
        code, _, err = self.pm(doc, "--device", "nope")
        self.assertEqual(code, 1)
        self.assertIn("have: dev, second", err)

    def test_servers(self):
        def server(**kw):
            return {"s": dict(kw)}

        cases = [
            (dict(comm="tcp", host="0.0.0.0", port=5020, framer="socket"),
             '(server (mode tcp) (port 5020) (bind "0.0.0.0"))'),
            (dict(comm="tcp", host="127.0.0.1", port=5020, framer="rtu"),
             '(server (mode rtu-tcp) (port 5020) (bind "127.0.0.1"))'),
            (dict(comm="tcp", host="127.0.0.1", port=5020, framer="ascii"),
             '(server (mode ascii-tcp) (port 5020) (bind "127.0.0.1"))'),
            (dict(comm="udp", host="127.0.0.1", port=5021, framer="socket"),
             '(server (mode udp) (port 5021) (bind "127.0.0.1"))'),
            (dict(comm="udp", host="127.0.0.1", port=5021, framer="rtu"),
             '(server (mode rtu-udp) (port 5021) (bind "127.0.0.1"))'),
            (dict(comm="udp", host="127.0.0.1", port=5021, framer="ascii"),
             '(server (mode ascii-udp) (port 5021) (bind "127.0.0.1"))'),
            (dict(comm="serial", port="/dev/ttyS1", framer="rtu", baudrate=19200, parity="E", stopbits=2),
             '(server (mode rtu-device) (path "/dev/ttyS1") (baud 19200) (parity even) (stop-bits 2))'),
            (dict(comm="serial", port="/dev/ttyS1", framer="ascii", baudrate=9600, bytesize=7, parity="E"),
             '(server (mode ascii-device) (path "/dev/ttyS1") (baud 9600) (parity even) (data-bits 7))'),
        ]
        for srv, want in cases:
            with self.subTest(want=want):
                code, out, err = self.pm(pm_doc(servers=server(**srv)))
                self.assertEqual(code, 0, err)
                self.assertIn(want, out)

    def test_unmappable_server_warns(self):
        code, out, err = self.pm(pm_doc(servers={"s": {"comm": "tcp", "port": 1, "host": "x", "framer": "binary"}}))
        self.assertEqual(code, 0)
        self.assertNotIn("(server", out)
        self.assertIn("no mogbus mode", err)

    def test_tls_server(self):
        _, out, err = self.pm(pm_doc(servers={"s": {"comm": "tls", "host": "0.0.0.0", "port": 5020,
                                                    "framer": "tls", "certfile": "c.pem", "keyfile": "k.pem"}}))
        self.assertIn('(tls (certificate "c.pem") (private-key "k.pem") (require-client-certificate off))', out)
        self.assertIn("without client certificates", err)

    def test_server_choice_identity_and_unit(self):
        servers = {"a": {"comm": "tcp", "host": "h", "port": 1, "framer": "socket", "device_id": 9,
                         "identity": {"VendorName": "V", "ModelName": "M", "Other": "x"}},
                   "b": {"comm": "udp", "host": "h", "port": 2, "framer": "socket"}}
        code, out, err = self.pm(pm_doc(servers=servers))
        self.assertIn("(port 1)", out)
        self.assertIn("(unit-id 9)", out)
        self.assertIn('(vendor-name "V")', out)
        self.assertIn('(model-name "M")', out)
        self.assertIn("server_list has 2 entries", err)
        code, out, err = self.pm(pm_doc(servers=servers), "--server-name", "b")
        self.assertIn("(mode udp)", out)
        self.assertIn("(unit-id 1)", out)
        code, out, _ = self.pm(pm_doc(servers=servers), "--no-server")
        self.assertNotIn("(server", out)

    def test_any_unit_by_default_and_exact_unit(self):
        doc = pm_doc({"uint16": [{"addr": 1, "value": 5}]})
        code, out, _ = self.pm(doc)
        self.assertIn("(any-unit on)", out)
        code, out, _ = self.pm(doc, "--exact-unit")
        self.assertNotIn("any-unit", out)
        servers = {"a": {"comm": "tcp", "host": "h", "port": 1, "framer": "socket", "device_id": 9}}
        code, out, _ = self.pm(pm_doc(servers=servers))
        self.assertNotIn("(any-unit", out)
        self.assertIn("(unit-id 9)", out)

    def test_all_devices(self):
        doc = pm_doc({"uint16": [{"addr": 1, "value": 11}]})
        second = pm_doc({"uint16": [{"addr": 2, "value": 22}]})["device_list"]["dev"]
        doc["device_list"]["second"] = second
        doc["device_list"]["third"] = second
        code, out, err = self.pm(doc, "--all-devices", "--unit-id", "5")
        self.assertEqual(code, 0, err)
        self.assertEqual(re.findall(r'\(name "(\w+)"\)', out), ["dev", "second", "third"])
        self.assertEqual(re.findall(r"\(unit-id (\d+)\)", out), ["5", "6", "7"])
        self.assertNotIn("any-unit", out)
        self.assertIn("(holding 2 (access read-only) (static 22)", out)
        self.assertIn("3 devices", err)
        self.assertIn("unit ids 5-7", err)
        code, out, _ = self.pm(pm_doc({"uint16": [{"addr": 1, "value": 11}]}), "--all-devices")
        self.assertIn("(any-unit on)", out)
        code, _, err = self.pm(doc, "--all-devices", "--device", "dev")
        self.assertEqual(code, 1)
        self.assertIn("exclude each other", err)
        if mogbus_bin():
            code, out, _ = self.pm(doc, "--all-devices")
            r = subprocess.run([mogbus_bin(), "--check", self.write("multi.scm", out)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_strict(self):
        code, _, _ = self.pm(pm_doc({"uint16": [{"addr": 1, "action": "my_custom"}]}), "--strict")
        self.assertEqual(code, 1)

    def test_not_a_pymodbus_file(self):
        code, _, err = self.pm({"hello": 1})
        self.assertEqual(code, 1)
        self.assertIn("device_list", err)
        path = self.write("bad.json", "{")
        code, _, err = self.run_tool("pymodbus", path)
        self.assertEqual(code, 1)
        self.assertIn("not valid JSON", err)


class Examples(ImportCase):
    """The committed .scm files stay in sync with the importer, and mogbus accepts them."""

    COMMANDS = {
        "meter-map.scm": ["csv", "meter-map.csv", "--server", "tcp", "--port", "1502", "--name", "Acme PM-300"],
        "pymodbus-setup.scm": ["pymodbus", "pymodbus-setup.json", "--device", "plant",
                               "--server-name", "main"],
    }

    def test_regenerating_gives_identical_output(self):
        for scm, argv in self.COMMANDS.items():
            with self.subTest(scm=scm):
                argv = [ROOT / "examples" / "import" / a if a.endswith((".csv", ".json")) else a
                        for a in argv]
                code, out, _ = self.run_tool(*argv)
                self.assertEqual(code, 0)
                self.assertEqual(out, (ROOT / "examples" / "import" / scm).read_text())

    @unittest.skipUnless(mogbus_bin(), "mogbus binary not built")
    def test_mogbus_accepts_the_examples(self):
        for scm in self.COMMANDS:
            with self.subTest(scm=scm):
                r = subprocess.run([mogbus_bin(), "--check=verbose", str(ROOT / "examples" / "import" / scm)],
                                   capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertIn("config OK", r.stdout)

    @unittest.skipUnless(mogbus_bin(), "mogbus binary not built")
    def test_check_flag(self):
        os.environ["MOGBUS_BIN"] = mogbus_bin()
        self.addCleanup(os.environ.pop, "MOGBUS_BIN", None)
        code, _, err = self.csv("Address,Type\n0,float32\n", "--check")
        self.assertEqual(code, 0, err)
        self.assertIn("config OK", err)

    @unittest.skipUnless(mogbus_bin(), "mogbus binary not built")
    def test_check_failure_is_reported(self):
        os.environ["MOGBUS_BIN"] = mogbus_bin()
        self.addCleanup(os.environ.pop, "MOGBUS_BIN", None)
        code, _, err = self.csv("Address\n0\n", "--check", "--unit-id", "999")
        self.assertEqual(code, 1)

    @unittest.skipUnless(mogbus_bin(), "mogbus binary not built")
    def test_every_layout_and_type_passes_check(self):
        rows = ["Address,Type,Layout"]
        addr = 0
        for t in ("u16", "s16", "u32", "s32", "float32", "u64", "s64", "float64"):
            for lay in ("ABCD", "CDAB", "BADC", "DCBA"):
                rows.append("%d,%s,%s" % (addr, t, lay))
                addr += 4
        rows.append("%d,string(10),CDAB" % addr)
        path = self.write("all.csv", "\n".join(rows) + "\n")
        out = Path(self.tmp.name) / "all.scm"
        self.assertEqual(self.run_tool("csv", path, "--base", "0", "-o", out)[0], 0)
        r = subprocess.run([mogbus_bin(), "--check", str(out)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
