#!/usr/bin/env python3
"""mogbus_import.py - turn a register map you already have into a mogbus config.

Python 3.9+, standard library only. Subcommands:

  mogbus_import.py csv FILE        a vendor register map as a CSV file
  mogbus_import.py pymodbus FILE   a pymodbus simulator JSON configuration (pymodbus 3.x)

The config goes to stdout, or to -o FILE. Warnings (unsupported or ambiguous input) go to stderr
and into the output as ';' comments; --strict turns them into errors. --check runs
$MOGBUS_BIN (default ./mogbus) --check on the result. See docs/user/import/import-overview.md.
"""
import argparse
import csv
import io
import json
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field

TABLES = ("coil", "discrete", "input", "holding")
TABLE_TITLE = {"coil": "Coils (FC 01/05/15)", "discrete": "Discrete inputs (FC 02)",
               "input": "Input registers (FC 04)", "holding": "Holding registers (FC 03/06/16)"}
WIDTH = {"u16": 1, "s16": 1, "u32": 2, "s32": 2, "float32": 2, "u64": 4, "s64": 4, "float64": 4}
INT_RANGE = {"u16": (0, 65535), "s16": (-32768, 32767), "u32": (0, 2**32 - 1),
             "s32": (-2**31, 2**31 - 1), "u64": (0, 2**64 - 1), "s64": (-2**63, 2**63 - 1)}
MAX_POINTS = 8192
NET_MODES = ("tcp", "rtu-tcp", "ascii-tcp", "udp", "rtu-udp", "ascii-udp")
DEVICE_MODES = ("rtu-device", "ascii-device")
PTY_MODES = ("rtu-pty", "ascii-pty")

LAYOUTS = {"ABCD": ("high-first", "big"), "CDAB": ("low-first", "big"),
           "BADC": ("high-first", "little"), "DCBA": ("low-first", "little")}
LAYOUT_DIGITS = {"1234": "ABCD", "3412": "CDAB", "2143": "BADC", "4321": "DCBA"}

# field -> header aliases (matched case-insensitively, ignoring spaces, '_', '-' and a
# trailing "(...)" such as "Address (dec)").
ALIASES = {
    "address": ["address", "register", "reg", "offset", "addr", "modbus address",
                "register address", "reg address", "register number", "reg no"],
    "space": ["table", "space", "register type", "type of register", "function code", "fc",
              "object type", "reg type", "modbus table", "function"],
    "type": ["type", "data type", "format", "datatype", "value type", "data format"],
    "layout": ["layout", "endianness", "endian", "swap", "byte layout", "byte sequence", "register order"],
    "word_order": ["word order", "word swap"],
    "byte_order": ["byte order", "byte swap"],
    "name": ["name", "parameter", "signal", "tag", "point", "register name", "parameter name",
             "variable", "label"],
    "description": ["description", "desc", "comment", "comments", "remarks", "notes",
                    "details", "meaning"],
    "unit": ["unit", "units", "uom", "engineering unit", "eu"],
    "default": ["default", "value", "initial", "initial value", "default value", "init",
                "preset"],
    "access": ["access", "rw", "r/w", "read/write", "permission", "permissions", "read write"],
    "scale": ["scale", "multiplier", "factor", "gain", "scaling", "scale factor", "coefficient"],
    "length": ["length", "size", "len", "registers", "count", "number of registers",
               "no of registers", "words", "qty", "quantity", "regs", "reg count"],
}

TYPE_MAP = {}
for _t, _names in {
    "u16": "u16 uint16 uint16t word unsigned uint unsignedint unsignedshort ushort unsigned16 "
           "int16u unsignedinteger",
    "s16": "s16 int16 int16t short signed int integer sint signedint signedshort signed16 "
           "int16s signedinteger",
    "u32": "u32 uint32 uint32t udint dword unsignedlong ulong unsigned32 unsignedlongint",
    "s32": "s32 int32 int32t dint long signedlong signed32 longint",
    "float32": "float float32 real ieee754 ieee single f32 ieee754float ieee754single",
    "u64": "u64 uint64 ulint lword unsignedlonglong ulonglong unsigned64",
    "s64": "s64 int64 lint longlong signed64",
    "float64": "float64 double lreal f64 ieee754double",
    "bool": "bool bit boolean flag binary onoff digital",
    "string": "string ascii char chars text str chararray visiblestring asciistring utf8",
}.items():
    for _n in _names.split():
        TYPE_MAP[_n] = _t

TRUE_WORDS = ("on", "true", "yes", "1", "high", "set", "enabled")
FALSE_WORDS = ("off", "false", "no", "0", "low", "clear", "disabled")
NONE_WORDS = ("", "-", "none", "no", "n", "false", "0", "off", "normal", "standard",
              "default", "n/a", "na", "no swap")


class Report:
    def __init__(self):
        self.warnings = []
        self.errors = []

    def warn(self, msg):
        if msg not in self.warnings:
            self.warnings.append(msg)

    def error(self, msg):
        self.errors.append(msg)


@dataclass
class Point:
    table: str
    addr: int
    type: str = None            # None for coil/discrete
    word_order: str = "high-first"
    byte_order: str = "big"
    length: int = 0             # strings: registers
    padding: str = "nul"
    read_only: bool = False
    value: object = None
    extra: list = field(default_factory=list)   # behaviour forms that replace (static ...)
    count: int = 1
    note: str = ""
    label: str = ""
    src: str = ""               # pymodbus: origin text; the note is built from it when merging
    noun: str = "cell"
    first: int = 0
    last: int = 0

    @property
    def width(self):
        if self.type is None:
            return 1
        return self.length if self.type == "string" else WIDTH[self.type]


@dataclass
class Result:
    name: str
    unit_id: int
    points: list
    source: str
    device_items: list = field(default_factory=list)
    server: str = ""
    info: list = field(default_factory=list)


# ---------------------------------------------------------------- formatting

def q(text):
    text = re.sub(r"[\r\n\t]+", " ", str(text))
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def f32_text(x):
    """Shortest decimal that reads back as the same float32."""
    for p in range(1, 10):
        s = "%.*g" % (p, x)
        if struct.unpack(">f", struct.pack(">f", float(s)))[0] == x:
            return s
    return repr(x)


def num(v):
    if isinstance(v, bool):
        return "on" if v else "off"
    if isinstance(v, int):
        return str(v)
    if v != v:
        return "+nan.0"
    if math.isinf(v):
        return "+inf.0" if v > 0 else "-inf.0"
    if v == int(v) and abs(v) < 1e15:
        return str(int(v))
    return repr(v)


def static_text(p):
    v = p.value
    if p.type is None:
        return "(static %s)" % ("on" if v else "off")
    if p.type == "string":
        return "(static %s)" % q(v)
    if p.type == "u64" and v > 2**63 - 1:
        return "(static #x%X)" % v
    if p.type == "float32" and isinstance(v, float) and v == v and not math.isinf(v):
        return "(static %s)" % f32_text(v)
    return "(static %s)" % num(v)


def ramp_text(lo, hi, every_ms, mode="wrap", step=1):
    return "(ramp (from %s) (to %s) (step %s) (every-ms %d) (mode %s))" % (
        num(lo), num(hi), num(step), every_ms, mode)


def render_point(p):
    parts = []
    if p.type not in (None, "u16"):
        parts.append("(type %s)" % p.type)
    if p.type in WIDTH and WIDTH[p.type] > 1 and p.word_order != "high-first":
        parts.append("(word-order %s)" % p.word_order)
    if p.type is not None and p.byte_order != "big":
        parts.append("(byte-order %s)" % p.byte_order)
    if p.type == "string":
        parts.append("(length %d)" % p.length)
        if p.padding != "nul":
            parts.append("(padding %s)" % p.padding)
    if p.read_only:
        parts.append("(access read-only)")
    if p.extra:
        parts.extend(p.extra)
    elif p.value is not None:
        parts.append(static_text(p))
    if p.count > 1:
        parts.append("(count %d)" % p.count)
    if p.note:
        parts.append("(note %s)" % q(p.note))
    lines = ["(%s %d" % (p.table, p.addr)]
    for part in parts:
        if len(lines[-1]) + 1 + len(part) > 100:
            lines.append("    " + part)
        else:
            lines[-1] += " " + part
    lines[-1] += ")"
    return lines


def server_form(mode, port=None, bind=None, path=None, baud=None, extra=()):
    parts = ["(mode %s)" % mode]
    if port is not None:
        parts.append("(port %d)" % port)
    if bind is not None:
        parts.append("(bind %s)" % q(bind))
    if path is not None:
        parts.append("(path %s)" % q(path))
    if baud is not None:
        parts.append("(baud %d)" % baud)
    parts.extend(extra)
    return "(server " + " ".join(parts) + ")"


def render_config(res, rep, tool_line):
    out = ["; " + tool_line, "; Source: " + res.source,
           "; %d points. Review the notes and warnings below, then check it with:" % len(res.points),
           ";   ./mogbus --check THIS-FILE.scm"]
    if res.info:
        out.append(";")
        out.append("; Notes:")
        out.extend(";   " + i for i in res.info)
    if rep.warnings:
        out.append(";")
        out.append("; Warnings:")
        out.extend(";   " + w for w in rep.warnings)
    out.append("")
    if res.server:
        out += [res.server, ""]
    out.append("(device")
    out.append("  (name %s)" % q(res.name[:63]))
    out.append("  (unit-id %d)" % res.unit_id)
    for item in res.device_items:
        out.append("  " + item)
    for table in TABLES:
        pts = sorted((p for p in res.points if p.table == table), key=lambda p: p.addr)
        if not pts:
            continue
        out += ["", "  ; " + TABLE_TITLE[table]]
        for p in pts:
            out.extend("  " + ln for ln in render_point(p))
    out[-1] += ")"
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------- parsing helpers

def norm(s):
    return re.sub(r"[^a-z0-9]+", "", s.lower())


def norm_variants(s):
    return {norm(s), norm(re.sub(r"[\(\[].*?[\)\]]", "", s))} - {""}


def parse_bool(t):
    t = t.strip().lower()
    if t in TRUE_WORDS:
        return True
    if t in FALSE_WORDS:
        return False
    return None


def parse_number(t):
    t = t.strip()
    low = t.lower()
    if low in ("nan", "+nan"):
        return float("nan")
    if low in ("inf", "+inf", "infinity"):
        return float("inf")
    if low in ("-inf", "-infinity"):
        return float("-inf")
    try:
        if re.fullmatch(r"[+-]?(0x|#x)[0-9a-f]+", low):
            return int(low.replace("#x", "0x"), 16)
        if re.fullmatch(r"[+-]?\d+", low):
            return int(low)
        return float(low.replace(",", "."))
    except ValueError:
        return None


def parse_address(t):
    """Returns (number, kind) with kind plain|hex|bit, or None."""
    t = t.strip()
    low = t.lower()
    if re.fullmatch(r"\d+", low):
        return int(low), "plain"
    m = re.fullmatch(r"(\d+)\.(\d+)", low)
    if m:
        return (int(m.group(1)), "plain") if int(m.group(2)) == 0 else (int(m.group(1)), "bit")
    m = re.fullmatch(r"(?:0x|#x|\$)([0-9a-f]+)|([0-9a-f]+)h", low)
    if m:
        return int(m.group(1) or m.group(2), 16), "hex"
    return None


def modicon_split(text):
    """'40001' -> (4, 1), '400001' -> (4, 1); None when it is not Modicon-shaped."""
    if re.fullmatch(r"[0-4]\d{4}", text):
        pre, off = int(text[0]), int(text[1:])
    elif re.fullmatch(r"[0-4]\d{5}", text):
        pre, off = int(text[0]), int(text[1:])
    else:
        return None
    if pre == 2 or off < 1 or off > 65536:
        return None
    return pre, off


MODICON_TABLE = {0: "coil", 1: "discrete", 3: "input", 4: "holding"}


def parse_space(t):
    """Table name for a space/function-code cell, or None."""
    low = t.strip().lower()
    key = re.sub(r"[^a-z0-9]+", "", low)
    if re.fullmatch(r"[0134]x", key):
        return {"0": "coil", "1": "discrete", "3": "input", "4": "holding"}[key[0]]
    if "discrete" in low or "input status" in low or "digital input" in low or key in ("di", "1xdi"):
        return "discrete"
    if "coil" in low or "digital output" in low or "output status" in low or key == "do":
        return "coil"
    if "holding" in low or key == "hr" or "analog output" in low or "output reg" in low or key == "ao":
        return "holding"
    if "input" in low or key in ("ir", "ai") or "analog input" in low:
        return "input"
    if key in ("coils", "output"):
        return "coil" if key == "coils" else "holding"
    m = re.fullmatch(r"(?:fc|functioncode|function)?(0x[0-9a-f]+|\d+)", key)
    if m:
        try:
            fc = int(m.group(1), 16) if m.group(1).startswith("0x") else int(m.group(1))
        except ValueError:
            return None
        return {1: "coil", 2: "discrete", 3: "holding", 4: "input", 5: "coil", 6: "holding",
                15: "coil", 16: "holding", 22: "holding", 23: "holding"}.get(fc)
    return None


def canon_type(text):
    """Returns (type, string_registers, layout_token); type is None when unknown."""
    low = text.strip().lower()
    layout = None
    m = re.search(r"\b(abcd|cdab|badc|dcba)\b", low)
    if m:
        layout = m.group(1).upper()
        low = (low[:m.start()] + low[m.end():]).strip()
    m = (re.search(r"(string|ascii|char|chars|text|str)\s*[\(\[]\s*(\d+)\s*([a-z]*)\s*[\)\]]", low) or
         re.search(r"(string|ascii|char|chars|text|str)\s+(\d+)\s*([a-z]*)\s*$", low))
    if m:
        n = int(m.group(2))
        regs = n if m.group(3).startswith(("reg", "word", "w")) else (n + 1) // 2
        return "string", max(regs, 1), layout
    key = norm(re.sub(r"[\(\[].*?[\)\]]", "", low))
    if key in TYPE_MAP:
        t = TYPE_MAP[key]
        return t, None, layout
    bits = re.search(r"(16|32|64)", key)
    if bits and ("int" in key or "signed" in key or "float" in key or "word" in key or "real" in key):
        n = bits.group(1)
        if "float" in key or "real" in key or "ieee" in key:
            return ("float32" if n == "32" else "float64" if n == "64" else None), None, layout
        unsigned = "unsigned" in key or key.startswith("u")
        return ("u" if unsigned else "s") + n, None, layout
    return None, None, layout


def parse_layout(text, field_name, rep, where):
    """(word_order, byte_order); None where the cell says nothing."""
    t = text.strip().lower()
    if t in NONE_WORDS:
        return ("high-first", "big") if field_name == "layout" else (None, None)
    letters = re.sub(r"[^a-z0-9]", "", t)
    letters = LAYOUT_DIGITS.get(letters, letters.upper())
    if letters in LAYOUTS:
        return LAYOUTS[letters]
    swap = "swap" in t or t in ("yes", "true", "1", "on")
    if "mid" in t:
        return LAYOUTS["CDAB"] if "little" in t else LAYOUTS["BADC"]
    if swap:
        w, b = "word" in t, "byte" in t
        if w and b:
            return LAYOUTS["DCBA"]
        if w:
            return LAYOUTS["CDAB"]
        if b:
            return LAYOUTS["BADC"]
        if field_name == "word_order":
            return ("low-first", None)
        if field_name == "byte_order":
            return (None, "little")
        rep.warn("%s: swap %r does not say words or bytes; assuming word swap (CDAB)" % (where, text))
        return LAYOUTS["CDAB"]
    little = "little" in t or t in ("le", "lsb first", "lsb")
    big = "big" in t or t in ("be", "msb first", "msb")
    if little or big:
        if field_name == "word_order":
            return ("low-first" if little else "high-first", None)
        if field_name == "byte_order":
            return (None, "little" if little else "big")
        return LAYOUTS["DCBA"] if little else LAYOUTS["ABCD"]
    if "lsw" in t or "low" in t:
        return ("low-first", None)
    if "msw" in t or "high" in t:
        return ("high-first", None)
    rep.warn("%s: unknown byte layout %r; using the default" % (where, text))
    return (None, None)


def parse_access(t, rep, where):
    low = re.sub(r"[^a-z]", "", t.lower())
    if low in ("r", "ro", "read", "readonly"):
        return "ro"
    if low in ("w", "wo", "write", "writeonly", "rw", "wr", "readwrite", "writeread"):
        return "rw"
    rep.warn("%s: unknown access %r ignored" % (where, t))
    return None


def conv_value(text, p, rep, where):
    """Static value of a default cell for point p, or None."""
    t = text.strip()
    if t == "":
        return None
    if p.type is None:
        b = parse_bool(t)
        if b is None:
            rep.warn("%s: default %r is not on/off; ignored" % (where, text))
        return b
    if p.type == "string":
        clean = "".join(c if 0x20 <= ord(c) < 0x7F else "?" for c in t)
        if clean != t:
            rep.warn("%s: non-ASCII characters in the string default replaced by '?'" % where)
        if len(clean) > 2 * p.length:
            rep.warn("%s: string default longer than %d bytes; truncated" % (where, 2 * p.length))
            clean = clean[:2 * p.length]
        return clean
    v = parse_number(t)
    if v is None:
        rep.warn("%s: default %r is not a number; ignored" % (where, text))
        return None
    if p.type in ("float32", "float64"):
        return float(v)
    if isinstance(v, float):
        if v != v or math.isinf(v):
            rep.warn("%s: default %r is not finite for an integer type; ignored" % (where, text))
            return None
        if v != int(v):
            rep.warn("%s: default %r rounded to a whole number" % (where, text))
        v = int(round(v))
    lo, hi = INT_RANGE[p.type]
    if not lo <= v <= hi:
        rep.warn("%s: default %d is outside %s; clamped" % (where, v, p.type))
        v = min(max(v, lo), hi)
    return v


# ---------------------------------------------------------------- CSV

def read_text(path):
    with open(path, "rb") as f:
        raw = f.read()
    try:
        return raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        return raw.decode("latin-1")


def match_columns(row, overrides):
    names = [norm_variants(c) for c in row]
    cols = {}
    taken = set()
    for fld, header in overrides.items():
        for i, n in enumerate(names):
            if header in n and i not in taken:
                cols[fld] = i
                taken.add(i)
                break
    for fld, aliases in ALIASES.items():
        if fld in cols:
            continue
        wanted = {norm(a) for a in aliases}
        for i, n in enumerate(names):
            if i not in taken and n & wanted:
                cols[fld] = i
                taken.add(i)
                break
    return cols


def parse_map(spec):
    out = {}
    for item in filter(None, (s.strip() for s in (spec or "").split(","))):
        if "=" not in item:
            sys.exit("mogbus_import: --map wants field=Header, got %r" % item)
        f, h = item.split("=", 1)
        f = f.strip().lower().replace("-", "_")
        if f not in ALIASES:
            sys.exit("mogbus_import: --map: unknown field %r (fields: %s)" % (f, ", ".join(ALIASES)))
        out[f] = norm(h)
    return out


def detect(text, overrides):
    """Picks the delimiter and header row. Returns (rows, header_index, cols, header_row)."""
    best = None
    for delim in (",", ";", "\t"):
        reader = csv.reader(io.StringIO(text, newline=""), delimiter=delim)
        rows = []
        for row in reader:
            rows.append((reader.line_num, row))
        for i, (_, row) in enumerate(rows):
            cols = match_columns(row, overrides)
            first_row = not any(any(c.strip() for c in r) for _, r in rows[:i])
            if "address" in cols and (len(cols) >= 2 or first_row):
                if best is None or len(cols) > len(best[2]):
                    best = (rows, i, cols)
                break
    return best


def convert_csv(args, rep):
    text = read_text(args.file)
    overrides = parse_map(args.map)
    found = detect(text, overrides)
    if not found:
        rep.error("no header row found: need a column named like address, register or offset "
                  "plus at least one more known column (see docs/user/import/import-overview.md; use --map)")
        return None
    rows, hdr, cols = found
    header = rows[hdr][1]

    def cell(row, f):
        i = cols.get(f)
        return row[i].strip() if i is not None and i < len(row) else ""

    recs = []
    for ln, row in rows[hdr + 1:]:
        if not any(c.strip() for c in row):
            continue
        if row[0].strip().startswith(("#", ";")):
            continue
        addr_text = cell(row, "address")
        if not addr_text:
            if sum(1 for c in row if c.strip()) >= 3:
                rep.warn("line %d: no address; row skipped" % ln)
            continue
        recs.append((ln, row, addr_text))

    parsed = []
    for ln, row, addr_text in recs:
        a = parse_address(addr_text)
        if a is None:
            rep.warn("line %d: cannot read address %r; row skipped" % (ln, addr_text))
            continue
        if a[1] == "bit":
            rep.warn("line %d: bit address %r inside a register is not supported; row skipped"
                     % (ln, addr_text))
            continue
        parsed.append((ln, row, addr_text, a))

    plain = [t for _, _, t, a in parsed if a[1] == "plain"]
    shaped = [modicon_split(t) is not None for t in plain]
    if args.modicon == "yes":
        modicon = True
    elif args.modicon == "no":
        modicon = False
    else:
        modicon = bool(plain) and all(shaped) and len(plain) == len(parsed)
    if args.modicon == "auto" and not modicon and any(shaped) and plain:
        rep.warn("some addresses look like Modicon numbers (40001) but not all do; "
                 "treating every address as a plain address (--modicon yes to override)")
    if args.base == "auto":
        if modicon:
            base = 1
        else:
            nums = [a[0] for _, _, _, a in parsed]
            base = 1 if nums and min(nums) == 1 else 0
            if base == 1:
                rep.warn("smallest address is 1 and there is no 0: assuming 1-based addresses "
                         "(subtracting 1); use --base 0 if the map is 0-based")
    else:
        base = int(args.base)

    pts = []
    for ln, row, addr_text, (number, kind) in parsed:
        name = cell(row, "name")
        where = "line %d%s" % (ln, " (%s)" % name if name else "")
        prefix = None
        offset = number
        if modicon and kind == "plain":
            split = modicon_split(addr_text)
            if split is None:
                rep.error("%s: address %s is not a Modicon address (--modicon no to read it as plain)"
                          % (where, addr_text))
                continue
            prefix, offset = split
            if prefix not in MODICON_TABLE:
                rep.error("%s: Modicon prefix %d is not a table" % (where, prefix))
                continue
        wire = offset - base
        if wire < 0:
            rep.error("%s: address %s is below the base %d" % (where, addr_text, base))
            continue

        ptype, str_regs, layout_tok = canon_type(cell(row, "type"))
        if cell(row, "type") and ptype is None:
            rep.warn("%s: unknown data type %r; row skipped" % (where, cell(row, "type")))
            continue
        if ptype is None:
            ptype = args.type if not cell(row, "type") else ptype

        access = parse_access(cell(row, "access"), rep, where) if cell(row, "access") else None

        table = None
        if cell(row, "space"):
            table = parse_space(cell(row, "space"))
            if table is None:
                rep.warn("%s: unknown table %r; falling back to the address/--space" %
                         (where, cell(row, "space")))
        if table is None and prefix is not None:
            table = MODICON_TABLE[prefix]
        elif table is not None and prefix is not None and table != MODICON_TABLE[prefix]:
            rep.warn("%s: table column says %s but the Modicon address %s says %s; using the column"
                     % (where, table, addr_text, MODICON_TABLE[prefix]))
        if table is None:
            if ptype == "bool":
                table = "discrete" if access == "ro" else "coil"
            else:
                table = args.space

        p = Point(table=table, addr=wire, label=where)
        if table in ("coil", "discrete"):
            if ptype not in (None, "bool") and cell(row, "type"):
                rep.warn("%s: type %s ignored in the %s table (on/off only)" % (where, ptype, table))
        else:
            if ptype == "bool":
                rep.warn("%s: bool in the %s table: a register cannot hold a bit here; "
                         "stored as u16 0/1" % (where, table))
                ptype = "u16"
            p.type = ptype

        if p.type == "string":
            length = str_regs
            if length is None and cell(row, "length"):
                m = re.match(r"\s*(\d+)\s*([a-z]*)", cell(row, "length").lower())
                if m:
                    n = int(m.group(1))
                    in_bytes = (m.group(2).startswith(("byte", "char")) or
                                (not m.group(2).startswith(("reg", "word")) and
                                 re.search(r"byte|char", header[cols["length"]].lower())))
                    length = (n + 1) // 2 if in_bytes else n
            if not length:
                rep.error("%s: string point needs a length (type STRING(n), or a length column)"
                          % where)
                continue
            if length > 123:
                rep.error("%s: string length %d registers exceeds 123" % (where, length))
                continue
            p.length = length
        elif cell(row, "length") and p.type in WIDTH:
            m = re.match(r"\s*(\d+)", cell(row, "length"))
            if m and int(m.group(1)) not in (WIDTH[p.type], 2 * WIDTH[p.type]):
                rep.warn("%s: length column says %s but %s uses %d registers; using the type" %
                         (where, cell(row, "length"), p.type, WIDTH[p.type]))

        wo, bo = args.layout_default
        for fld, tok in (("layout", layout_tok or ""), ("layout", cell(row, "layout")),
                         ("word_order", cell(row, "word_order")),
                         ("byte_order", cell(row, "byte_order"))):
            if not tok:
                continue
            w, b = parse_layout(tok, fld, rep, where)
            wo, bo = w or wo, b or bo
        if p.type is not None:
            p.byte_order = bo
            if p.type in WIDTH and WIDTH[p.type] > 1:
                p.word_order = wo

        p.value = conv_value(cell(row, "default"), p, rep, where)

        scale = cell(row, "scale")
        scale_text = ""
        if scale:
            sv = parse_number(scale)
            if sv is None or sv != 1:
                scale_text = "raw x %s" % scale
                rep.warn("%s: scale %s: mogbus stores raw register values; put the raw (scaled) "
                         "value in the default" % (where, scale))
        desc = cell(row, "description")
        note = name
        if desc and desc != name:
            note = "%s - %s" % (name, desc) if name else desc
        unit = cell(row, "unit")
        if unit:
            note = "%s (%s)" % (note, unit) if note else "(%s)" % unit
        if scale_text:
            note = "%s, %s" % (note, scale_text) if note else scale_text
        p.note = note

        if access == "ro" and table in ("coil", "holding"):
            p.read_only = True
        elif access == "rw" and table in ("input", "discrete"):
            rep.warn("%s: writable, but the %s table is read-only in mogbus (table kept)" %
                     (where, table))
        pts.append(p)

    check_overlaps(pts, rep)
    for p in pts:
        if p.addr + p.width - 1 > 65535:
            rep.error("%s: address %d + %d registers runs past 65535" % (p.label, p.addr, p.width))
    if len(pts) > MAX_POINTS:
        rep.error("%d points; a mogbus device holds at most %d" % (len(pts), MAX_POINTS))
    res = Result(name=args.name or os.path.splitext(os.path.basename(args.file))[0],
                 unit_id=args.unit_id, points=pts, source="CSV file %s" % os.path.basename(args.file))
    res.info.append("addresses: base %d%s" % (base, ", Modicon style" if modicon else ""))
    if args.server:
        res.server = make_server(args, rep)
    return res


def check_overlaps(points, rep):
    for table in TABLES:
        pts = sorted((p for p in points if p.table == table), key=lambda p: p.addr)
        prev = None
        for p in pts:
            if prev is not None and p.addr <= prev.addr + prev.width - 1:
                rep.error("%s overlaps %s in the %s table (address %d)" %
                          (p.label, prev.label, table, p.addr))
                if p.addr + p.width > prev.addr + prev.width:
                    prev = p
                continue
            prev = p


def make_server(args, rep):
    mode = args.server
    if mode not in NET_MODES + DEVICE_MODES + PTY_MODES:
        rep.error("unknown server mode %r" % mode)
        return ""
    if args.port is not None and mode not in NET_MODES:
        rep.error("--port is only valid for network modes")
    if mode in DEVICE_MODES and not args.path:
        rep.error("--server %s needs --path" % mode)
    return server_form(mode, args.port if mode in NET_MODES else None,
                       args.bind if mode in NET_MODES else None,
                       args.path if mode in DEVICE_MODES else None,
                       args.baud if mode not in ("tcp", "udp", "ascii-udp", "rtu-udp") else None)


# ---------------------------------------------------------------- pymodbus

@dataclass
class Cell:
    type: str
    value: int = 0
    action: object = None
    params: object = None
    access: bool = False


def pm_ints(entry, what, rep):
    """int or [a, b] -> (a, b) or None."""
    if isinstance(entry, int) and not isinstance(entry, bool):
        return entry, entry
    if (isinstance(entry, list) and len(entry) == 2 and
            all(isinstance(x, int) and not isinstance(x, bool) for x in entry) and
            0 <= entry[0] <= entry[1]):
        return entry[0], entry[1]
    rep.error("pymodbus %s: bad address %r (want N or [first, last])" % (what, entry))
    return None


def convert_pymodbus(args, rep):
    try:
        data = json.loads(read_text(args.file))
    except ValueError as e:
        rep.error("not valid JSON: %s" % e)
        return None
    devices = data.get("device_list") if isinstance(data, dict) else None
    if not isinstance(devices, dict) or not devices:
        rep.error("no device_list: this is not a pymodbus simulator configuration")
        return None
    names = list(devices)
    if args.device:
        if args.device not in devices:
            rep.error("no device %r in device_list (have: %s)" % (args.device, ", ".join(names)))
            return None
        dname = args.device
    else:
        dname = names[0]
        if len(names) > 1:
            rep.warn("device_list has %d devices; using %r, ignoring %s (--device NAME picks one)"
                     % (len(names), dname, ", ".join(repr(n) for n in names[1:])))
    dev = devices[dname]
    setup = dev.get("setup", {})
    shared = bool(setup.get("shared blocks", False))
    sizes = {k: int(setup.get(k + " size", 0)) for k in ("co", "di", "ir", "hr")}
    defaults = setup.get("defaults", {})
    dval = defaults.get("value", {})
    dact = defaults.get("action", {})
    sections = {"bits": 0, "uint16": 0, "uint32": 0, "float32": 0.0, "string": " "}
    cells = {}

    def put(idx, cell, what):
        if idx in cells:
            rep.error("pymodbus %s: cell %d is defined twice (pymodbus itself rejects this)" % (what, idx))
        cells[idx] = cell

    for sec, default in sections.items():
        for entry in dev.get(sec, []):
            d = entry if isinstance(entry, dict) else {"addr": entry}
            rng = pm_ints(d.get("addr"), sec, rep)
            if rng is None:
                continue
            first, last = rng
            value = d.get("value", dval.get(sec, default))
            action = d.get("action", dact.get(sec))
            params = d.get("parameters")
            what = "%s %s" % (sec, d.get("addr"))
            if sec in ("bits", "uint16"):
                if not isinstance(value, int) or not 0 <= value <= 65535:
                    rep.error("pymodbus %s: value %r is not a 16-bit integer" % (what, value))
                    continue
                for i in range(first, last + 1):
                    put(i, Cell(sec, value, action, params), what)
            elif sec == "uint32":
                if not isinstance(value, int) or not 0 <= value <= 2**32 - 1:
                    rep.error("pymodbus %s: value %r is not a 32-bit unsigned integer" % (what, value))
                    continue
                for i in range(first, last + 1, 2):
                    put(i, Cell(sec, value >> 16, action, params), what)
                    put(i + 1, Cell("next", value & 0xFFFF), what)
            elif sec == "float32":
                try:
                    raw = struct.pack(">f", float(value))
                except (OverflowError, TypeError, ValueError):
                    rep.error("pymodbus %s: value %r does not fit a float32" % (what, value))
                    continue
                hi, lo = struct.unpack(">HH", raw)
                for i in range(first, last + 1, 2):
                    put(i, Cell(sec, hi, action, params), what)
                    put(i + 1, Cell("next", lo), what)
            else:
                n = last - first + 1
                if not isinstance(value, str) or not value.isascii() or len(value) > 2 * n:
                    rep.error("pymodbus %s: value %r is not ASCII text of at most %d characters"
                              % (what, value, 2 * n))
                    continue
                raw = value.ljust(2 * n).encode("ascii")
                for k in range(n):
                    w = int.from_bytes(raw[2 * k:2 * k + 2], "big")
                    put(first + k, Cell("string" if k == 0 else "next", w, action if k == 0 else None,
                                        params if k == 0 else None), what)

    for entry in dev.get("write", []):
        rng = pm_ints(entry, "write", rep)
        for i in range(rng[0], rng[1] + 1) if rng else ():
            if i not in cells:
                rep.error("pymodbus write: cell %d is not defined (pymodbus itself rejects this)" % i)
            else:
                cells[i].access = True

    for entry in dev.get("repeat", []):
        src = pm_ints(entry.get("addr"), "repeat", rep)
        dst = pm_ints(entry.get("to"), "repeat", rep)
        if not src or not dst:
            continue
        k = src[0] - 1
        for i in range(dst[0], dst[1] + 1):
            k = src[0] if k >= src[1] else k + 1
            if k in cells:
                c = cells[k]
                cells[i] = Cell(c.type, c.value, c.action, c.params, c.access)
            else:
                cells.pop(i, None)

    pts = pm_points(cells, shared, sizes, args, rep)
    if len(sum(([p] * p.count for p in pts), [])) > MAX_POINTS:
        rep.error("more than %d points; a mogbus device holds at most %d" % (MAX_POINTS, MAX_POINTS))
    res = Result(name=args.name or dname, unit_id=args.unit_id or 1, points=pts,
                 source="pymodbus simulator file %s, device %r" % (os.path.basename(args.file), dname))
    if setup.get("shared blocks"):
        res.info.append("pymodbus serves one register array for all tables: every register is "
                        "emitted in both the holding and the input table, every bit cell in both "
                        "the coil and the discrete table.")
    ident = {"VendorName": "vendor-name", "ProductCode": "product-code", "VendorUrl": "vendor-url",
             "ProductName": "product-name", "ModelName": "model-name",
             "MajorMinorRevision": "major-minor-revision",
             "UserApplicationName": "user-application-name"}
    servers = data.get("server_list")
    if isinstance(servers, dict) and servers and not args.no_server:
        sname = args.server_name or next(iter(servers))
        if sname not in servers:
            rep.error("no server %r in server_list (have: %s)" % (sname, ", ".join(servers)))
        else:
            if not args.server_name and len(servers) > 1:
                rep.warn("server_list has %d entries; using %r (--server-name NAME picks one, "
                         "--no-server omits the section)" % (len(servers), sname))
            srv = servers[sname]
            res.server = pm_server(srv, sname, rep)
            if "device_id" in srv and not args.unit_id:
                res.unit_id = int(srv["device_id"])
            elif "device_id" not in srv:
                res.info.append("the pymodbus server answers every unit id; mogbus answers unit %d "
                                "only (--unit-id)" % res.unit_id)
            idn = srv.get("identity")
            if isinstance(idn, dict):
                for k, v in idn.items():
                    if k in ident:
                        res.device_items.append("(%s %s)" % (ident[k], q(v)))
    return res


def pm_server(srv, sname, rep):
    comm, framer = srv.get("comm"), srv.get("framer")
    kind = {"rtu": "rtu", "ascii": "ascii"}.get(framer)
    if comm in ("tcp", "udp", "tls") and (framer in ("socket", "tls") or kind):
        if comm == "tls":
            mode = "tcp"
        else:
            mode = comm if framer == "socket" else "%s-%s" % (kind, comm)
        extra = []
        if comm == "tls":
            if framer != "tls" or "certfile" not in srv or "keyfile" not in srv:
                rep.warn("server %r: tls needs framer tls, certfile and keyfile; server section omitted" % sname)
                return ""
            extra.append("(tls (certificate %s) (private-key %s) (require-client-certificate off))"
                         % (q(srv["certfile"]), q(srv["keyfile"])))
            rep.warn("server %r: tls emitted without client certificates; add (ca-certificate ...) "
                     "and remove (require-client-certificate off) to require them" % sname)
        elif framer == "tls":
            rep.warn("server %r: framer tls without comm tls; server section omitted" % sname)
            return ""
        return server_form(mode, srv.get("port"), srv.get("host"), extra=extra)
    if comm == "serial" and kind:
        extra = []
        parity = {"N": "none", "E": "even", "O": "odd"}.get(str(srv.get("parity", "N")).upper())
        if parity is None:
            rep.warn("server %r: parity %r is not supported; using none" % (sname, srv.get("parity")))
        elif parity != "none":
            extra.append("(parity %s)" % parity)
        if srv.get("stopbits") in (1, 2) and srv.get("stopbits") != 1:
            extra.append("(stop-bits %d)" % srv["stopbits"])
        size = srv.get("bytesize", 8)
        if kind == "ascii" and size in (7, 8):
            if size == 7:
                extra.append("(data-bits 7)")
        elif size != 8:
            rep.warn("server %r: bytesize %r is not supported; using 8" % (sname, size))
        if "port" not in srv:
            rep.warn("server %r: serial entry without port; server section omitted" % sname)
            return ""
        return server_form("%s-device" % kind, path=srv["port"], baud=srv.get("baudrate"), extra=extra)
    rep.warn("server %r: comm %r with framer %r has no mogbus mode; server section omitted"
             % (sname, comm, framer))
    return ""


def pm_behaviour(p, ctype, value, action, params, args, rep, where):
    """Sets p.value / p.extra from a pymodbus action. Registers only."""
    p.value = value
    if action is None:
        return
    params = params or {}
    tmax = {"u16": 65535, "u32": 2**32 - 1, "float32": 16777216}[p.type]
    ms = args.action_ms
    if action == "increment":
        lo = params.get("minval")
        hi = params.get("maxval")
        start = lo if lo is not None else value
        end = hi if hi is not None else tmax
        if lo is not None and value != lo:
            rep.warn("%s: increment starts at %s but minval is %s; the ramp starts at minval"
                     % (where, num(value), num(lo)))
        if not start < end:
            rep.warn("%s: increment range %s..%s is empty; static value kept" % (where, num(start), num(end)))
            return
        if p.type == "float32":
            start, end = float(start), float(end)
        p.extra = [ramp_text(start, end, ms)]
        p.value = None
    elif action == "random":
        lo, hi = params.get("minval", 1), params.get("maxval", 65536)
        if hi > lo:
            p.value = (lo + hi) / 2
            if p.type != "float32":
                p.value = int(round(p.value))
            p.extra = [static_text(p), "(noise (amplitude %s) (every-ms %d))" % (num((hi - lo) / 2), ms)]
    elif action == "uptime":
        p.extra = [ramp_text(1, tmax, 1000, "hold")]
        p.value = None
    else:
        rep.warn("%s: action %r has no mogbus equivalent; the initial value is kept static" %
                 (where, action))


def pm_points(cells, shared, sizes, args, rep):
    # Which mogbus table(s) does each cell index belong to, and at what address?
    co, di, ir = sizes["co"], sizes["di"], sizes["ir"]

    def bit_target(idx):
        if shared:
            return [("coil", idx * 16), ("discrete", idx * 16)]
        if idx < co:
            return [("coil", idx * 16)]
        if idx < co + di:
            return [("discrete", (idx - co) * 16)]
        return []

    def reg_target(idx):
        if shared:
            return [("holding", idx), ("input", idx)]
        if idx >= co + di + ir:
            return [("holding", idx - co - di - ir)]
        if idx >= co + di:
            return [("input", idx - co - di)]
        return []

    items = []   # (kind, idx, width, cell, value words)
    i = 0
    order = sorted(cells)
    seen = set()
    for i in order:
        if i in seen:
            continue
        c = cells[i]
        if c.type == "next":
            continue
        if c.type == "bits":
            items.append(("bits", i, 1, c, None))
        elif c.type == "uint16":
            items.append(("u16", i, 1, c, [c.value]))
        elif c.type in ("uint32", "float32"):
            nxt = cells.get(i + 1)
            if nxt is None or nxt.type != "next":
                rep.warn("pymodbus cell %d: %s without its second register; skipped" % (i, c.type))
                continue
            seen.add(i + 1)
            items.append(("u32" if c.type == "uint32" else "float32", i, 2, c, [c.value, nxt.value]))
        else:
            j = i + 1
            while cells.get(j) is not None and cells[j].type == "next":
                seen.add(j)
                j += 1
            items.append(("string", i, j - i, c, [cells[k].value for k in range(i, j)]))

    pts = []
    for kind, idx, width, c, words in items:
        where = "pymodbus %s at cell %d" % ({"bits": "bits", "u16": "uint16", "u32": "uint32",
                                              "float32": "float32", "string": "string"}[kind], idx)
        if kind == "bits":
            targets = bit_target(idx)
            if not targets:
                rep.warn("%s: not reachable by any function code in this layout; skipped" % where)
                continue
            if c.action:
                rep.warn("%s: action %r on bits is not supported; the value is kept static" %
                         (where, c.action))
            if idx * 16 + 15 > 65535:
                rep.warn("%s: coil addresses beyond 65535; skipped" % where)
                continue
            for table, base in targets:
                for k in range(16):
                    pts.append(Point(table=table, addr=base + k, value=bool(c.value >> k & 1),
                                     src="pymodbus bits", noun="bit", first=base + k,
                                     last=base + k, label=where,
                                     read_only=table == "coil" and not c.access))
            continue
        targets = reg_target(idx)
        if not targets:
            rep.warn("%s: not reachable by any function code in this layout; skipped" % where)
            continue
        for table, addr in targets:
            p = Point(table=table, addr=addr, label=where, src="pymodbus " + where.split()[1],
                      first=idx, last=idx, read_only=table == "holding" and not c.access)
            if kind == "string":
                raw = b"".join(w.to_bytes(2, "big") for w in words)
                p.type, p.length, p.padding = "string", width, "space"
                p.value = raw.decode("ascii", "replace").rstrip(" ") if c.action is None else \
                    raw.decode("ascii", "replace")
                if c.action:
                    rep.warn("%s: action %r on a string is not supported; kept static" % (where, c.action))
            else:
                p.type = {"u16": "u16", "u32": "u32", "float32": "float32"}[kind]
                if kind == "u16":
                    v = words[0]
                elif kind == "u32":
                    v = words[0] << 16 | words[1]
                else:
                    v = struct.unpack(">f", struct.pack(">HH", *words))[0]
                pm_behaviour(p, kind, v, c.action, c.params, args, rep, where)
                if c.action:
                    p.src += ", action %s%s" % (c.action, " (%s)" % ", ".join("%s %s" % kv for kv in sorted(c.params.items()))
                                                if c.params else "")
            pts.append(p)
    return group_points(pts)


def group_points(pts):
    """Merges adjacent identical points into one with (count N) and builds the notes."""
    out = []
    for p in sorted(pts, key=lambda p: (TABLES.index(p.table), p.addr)):
        prev = out[-1] if out else None
        if (prev is not None and prev.table == p.table and prev.type == p.type and
                prev.addr + prev.width * prev.count == p.addr and prev.value == p.value and
                prev.extra == p.extra and prev.length == p.length and
                prev.padding == p.padding and prev.read_only == p.read_only and prev.src == p.src and p.type != "string"):
            prev.count += 1
            prev.last = p.first
        else:
            out.append(p)
    for p in out:
        where = ("%s %d" % (p.noun, p.first) if p.first == p.last
                 else "%ss %d-%d" % (p.noun, p.first, p.last))
        p.note = "%s, %s" % (p.src.split(", action")[0], where) + \
            (", action" + p.src.split(", action", 1)[1] if ", action" in p.src else "")
    return out


# ---------------------------------------------------------------- CLI

def build_parser():
    ap = argparse.ArgumentParser(prog="mogbus_import.py", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(sp):
        sp.add_argument("file", help="input file")
        sp.add_argument("-o", "--output", help="write the config here instead of stdout")
        sp.add_argument("--strict", action="store_true", help="treat warnings as errors")
        sp.add_argument("--check", action="store_true",
                        help="run $MOGBUS_BIN (default ./mogbus) --check on the result")
        sp.add_argument("--unit-id", type=int, default=None, help="Modbus unit id (default 1)")
        sp.add_argument("--name", help="device name (default: file name or pymodbus device name)")

    c = sub.add_parser("csv", help="a vendor register map as CSV")
    common(c)
    c.add_argument("--map", help="column overrides: field=Header,... (fields: %s)" % ", ".join(ALIASES))
    c.add_argument("--base", choices=("0", "1", "auto"), default="auto",
                   help="first address of the map: 0 or 1 (default: auto)")
    c.add_argument("--modicon", choices=("auto", "yes", "no"), default="auto",
                   help="read 40001-style numbers as Modicon addresses (default: auto)")
    c.add_argument("--space", choices=TABLES, default="holding",
                   help="table when nothing else says (default holding)")
    c.add_argument("--type", default="u16", choices=sorted(set(TYPE_MAP.values()) - {"bool"}),
                   help="data type when the map has none (default u16)")
    c.add_argument("--layout", choices=sorted(LAYOUTS), default="ABCD",
                   help="byte layout when the map has none (default ABCD)")
    c.add_argument("--server", help="add a (server ...) section with this mode (tcp, rtu-tcp, ...)")
    c.add_argument("--port", type=int)
    c.add_argument("--bind")
    c.add_argument("--path", help="device path for rtu-device/ascii-device")
    c.add_argument("--baud", type=int)

    p = sub.add_parser("pymodbus", help="a pymodbus simulator JSON configuration")
    common(p)
    p.add_argument("--device", help="device_list entry to import (default: the first)")
    p.add_argument("--server-name", help="server_list entry to import (default: the first)")
    p.add_argument("--no-server", action="store_true", help="do not write a (server ...) section")
    p.add_argument("--action-ms", type=int, default=1000,
                   help="milliseconds per step for increment and random actions (default 1000)")
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)
    rep = Report()
    if args.cmd == "csv":
        args.layout_default = LAYOUTS[args.layout]
        if args.unit_id is None:
            args.unit_id = 1
        res = convert_csv(args, rep)
    else:
        res = convert_pymodbus(args, rep)
    for e in rep.errors:
        print("mogbus_import: error: " + e, file=sys.stderr)
    for w in rep.warnings:
        print("mogbus_import: warning: " + w, file=sys.stderr)
    if rep.errors or res is None:
        return 1
    if args.strict and rep.warnings:
        print("mogbus_import: --strict: %d warning(s)" % len(rep.warnings), file=sys.stderr)
        return 1
    text = render_config(res, rep, "Generated by tools/mogbus_import.py (%s)." % args.cmd)
    if args.output:
        with open(args.output, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    if args.check:
        path = args.output
        tmp = None
        if not path:
            tmp = tempfile.NamedTemporaryFile("w", suffix=".scm", delete=False)
            tmp.write(text)
            tmp.close()
            path = tmp.name
        try:
            r = subprocess.run([os.environ.get("MOGBUS_BIN", "./mogbus"), "--check", path],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            sys.stderr.write(r.stdout)
            if r.returncode != 0:
                return 1
        except OSError as e:
            print("mogbus_import: --check: cannot run mogbus (set MOGBUS_BIN): %s" % e, file=sys.stderr)
            return 1
        finally:
            if tmp:
                os.unlink(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
