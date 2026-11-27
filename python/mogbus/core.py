"""@file
@brief Python API of mogbus: devices, servers and helpers over libmogbus.so.

The package wraps the C library with `ctypes` (no compiled
extension, standard library only, Python 3.9 or newer). Everything below is importable from
`mogbus`; the names are the ones of `include/mogbus.h` in snake_case. See the
[Python tutorial](../../docs/user/python/python-start.md) for a walk-through.

## Names

Spaces (`"coil"`, `"discrete"`, `"input"`, `"holding"`), value types (`"u16"`, `"s16"`, `"u32"`,
`"s32"`, `"f32"`/`"float32"`, `"bool"`, `"u64"`, `"s64"`, `"f64"`/`"float64"`, `"string"`), word
orders (`"high-first"`, `"low-first"`), byte orders (`"big"`, `"little"`), ramp modes (`"wrap"`,
`"bounce"`, `"hold"`), server modes (`"tcp"`, `"rtu-tcp"`, `"ascii-udp"`, ...) and the other
enumerations are accepted as strings (case-insensitive, `-` or `_`), as the enum members, or as
plain ints. Anything else raises MogbusError listing the valid names.

## Library lookup

The library is searched in this order: the `MOGBUS_LIB` path (an error if it fails to load, no
fallback), a `libmogbus.so` inside the package (`make python` puts it there), `libmogbus.so` at
the checkout root when running from `python/`, then `ctypes.util.find_library("mogbus")`. The
import fails with a message listing what was tried, or when the major.minor version of the
library differs from the package. library_version() returns the version of the loaded library.

## Errors

Every failure raises MogbusError with the message of the C error buffer; a failed server creation
also sets `MogbusError.status`. Invalid arguments (a name that is not valid, a number out of
range, a closed context) raise it too, before the C library is called.

## Threading

The C library is not thread-safe, so every Context, Device and Server method takes one
re-entrant lock per context: once a server runs with Server.start(), any thread may read and write
device values. Server.stop() takes no lock. Logging is process-wide (set_log_sink()).

## C equivalents

The classes mirror `mogbus.h`: Context is `mogbus_create`/`mogbus_destroy`, Context.load_config
`mogbus_load_config`, Context.add_device/find_device `mogbus_add_device`/`mogbus_find_device`,
Device.add_point `mogbus_device_add_point`, Device.read/write `mogbus_device_read/write` (raw
registers), Device.get_value/set_value `mogbus_device_get_value/set_value` (decoded),
Device.get_string/set_string `mogbus_device_get_string/set_string`, Device.set_access_hook
`mogbus_device_set_access_hook`, Device.set_written_hook `mogbus_device_set_written_hook`, Device.add_function_code `mogbus_device_add_function_code`, Device.info `mogbus_device_info`, Context.serve
`mogbus_server_create`, Context.serve_config `mogbus_server_create_from_config`, Server.step/run/stop/close
`mogbus_server_step/run/stop/close`, Server.metrics `mogbus_server_metrics`, probe `mogbus_probe`,
rtu_timing `mogbus_rtu_timing` and the set_log_* functions `mogbus_set_log_level/format/sink`.

## Limits

Linux only (on Windows it works inside WSL2). The limits of the C API apply: 16 devices and 8
servers per context, a server serves the devices that exist when it is created, and machines
and faults can only be defined in config files. A TLS server needs a library built with TLS. The
package installs no signal handlers; a host process serving TLS should ignore `SIGPIPE`. The wheel
contains a native library, so build and install it on the machine that runs the tests.
"""
from __future__ import annotations

import contextlib
import ctypes
import dataclasses
import enum
import functools
import os
import socket
import tempfile
import threading
import time
import traceback
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, Iterator, List, NamedTuple, Optional, Sequence, Union

from . import _lib
from ._lib import lib

ERR_SIZE = 512


class MogbusError(Exception):
    """@brief A failed mogbus call.

    Raised for every error of the library and for invalid arguments.
    @param message The message of the C error buffer, or a description of the invalid argument.
    @param status The Status of a failed server creation (`INVALID`, `SERIAL`, `LISTEN`), else None.
    """

    def __init__(self, message: str, status: Optional["Status"] = None):
        super().__init__(message)
        self.status = status


class Space(enum.IntEnum):
    """@brief The four Modbus tables of a device.
    """
    ## Read/write bits.
    COIL = 0
    ## Read-only bits.
    DISCRETE = 1
    ## Read-only 16-bit registers.
    INPUT = 2
    ## Read/write 16-bit registers.
    HOLDING = 3


class AccessKind(enum.IntEnum):
    """@brief Whether a client access reads or writes.
    """
    ## A read request.
    READ = 0
    ## A write request.
    WRITE = 1


class AccessTarget(enum.IntEnum):
    """@brief What a client access touches; see Access.
    """
    ## A coil, discrete, input or holding table.
    TABLE = 0
    ## A file record (FC 14, 15).
    FILE_RECORD = 1
    ## A FIFO queue (FC 18).
    FIFO = 2
    ## A CANopen object (FC 2B/0D).
    CANOPEN = 3


class ValueType(enum.IntEnum):
    """@brief How a point's registers are decoded; `F32` and `F64` have the config names `FLOAT32` and `FLOAT64` as aliases.
    """
    ## Unsigned 16-bit, one register.
    U16 = 0
    ## Signed 16-bit, one register.
    S16 = 1
    ## Unsigned 32-bit, two registers.
    U32 = 2
    ## Signed 32-bit, two registers.
    S32 = 3
    ## IEEE 754 single, two registers.
    F32 = 4
    ## Alias of F32.
    FLOAT32 = 4  # alias of F32, as in the config file
    ## One bit (coils and discrete inputs).
    BOOL = 5
    ## Unsigned 64-bit, four registers.
    U64 = 6
    ## Signed 64-bit, four registers.
    S64 = 7
    ## IEEE 754 double, four registers.
    F64 = 8
    ## Alias of F64.
    FLOAT64 = 8  # alias of F64, as in the config file
    ## ASCII text in `length` registers (holding and input, static only).
    STRING = 9


class WordOrder(enum.IntEnum):
    """@brief Order of the registers of a 32 or 64-bit point.
    """
    ## The high word is at the lower address.
    HIGH_FIRST = 0
    ## The low word is at the lower address.
    LOW_FIRST = 1


class ByteOrder(enum.IntEnum):
    """@brief Order of the two bytes inside every register of a point; with WordOrder it gives the ABCD, CDAB, BADC and DCBA layouts.
    """
    ## High byte first in every register.
    BIG = 0
    ## The two bytes of every register swapped.
    LITTLE = 1


class RampMode(enum.IntEnum):
    """@brief What a ramp does when it reaches its end.
    """
    ## Jump back to the start after the end.
    WRAP = 0
    ## Turn around at the ends.
    BOUNCE = 1
    ## Stay at the end.
    HOLD = 2


class ClockField(enum.IntEnum):
    """@brief The wall-clock field a clock point shows.
    """
    ## Calendar year.
    YEAR = 0
    ## Month, 1-12.
    MONTH = 1
    ## Day of month.
    DAY = 2
    ## Day of week.
    WEEKDAY = 3
    ## Hour, 0-23.
    HOUR = 4
    ## Minute, 0-59.
    MINUTE = 5
    ## Second, 0-59.
    SECOND = 6
    ## Seconds since the Unix epoch (needs 32 bits, always UTC).
    UNIX = 7


class Mode(enum.IntEnum):
    """@brief The transport a server uses. The names are the config's mode names (`"rtu-tcp"` is `RTU_TCP`).
    """
    ## Modbus TCP (MBAP), optionally TLS.
    TCP = 0
    ## RTU on a new PTY; see Server.pty_name.
    RTU_PTY = 1
    ## RTU on the serial device `path`.
    RTU_DEVICE = 2
    ## RTU frames over TCP.
    RTU_TCP = 3
    ## ASCII on a new PTY; see Server.pty_name.
    ASCII_PTY = 4
    ## ASCII on the serial device `path`.
    ASCII_DEVICE = 5
    ## ASCII frames over TCP.
    ASCII_TCP = 6
    ## RTU on an open byte-stream fd you own.
    RTU_FD = 7
    ## ASCII on an open byte-stream fd you own.
    ASCII_FD = 8
    ## Modbus UDP: one MBAP ADU per datagram.
    UDP = 9
    ## One CRLF-terminated ASCII frame per datagram.
    ASCII_UDP = 10
    ## One RTU frame per UDP datagram.
    RTU_UDP = 11


class Parity(enum.IntEnum):
    """@brief Serial line parity; mark and space need CMSPAR support.
    """
    ## No parity.
    NONE = 0
    ## Even parity.
    EVEN = 1
    ## Odd parity.
    ODD = 2
    ## Mark parity.
    MARK = 3
    ## Space parity.
    SPACE = 4


class TlsVersion(enum.IntEnum):
    """@brief Minimum TLS version of a TLS server.
    """
    ## TLS 1.2 (`"1.2"`).
    V1_2 = 0
    ## TLS 1.3 (`"1.3"`).
    V1_3 = 1


class FaultKind(enum.IntEnum):
    """@brief The kinds of fault a device can have; see DeviceInfo.faults.
    """
    ## Delay the response.
    DELAY = 0
    ## Send no response.
    DROP = 1
    ## Answer with an exception code.
    EXCEPTION = 2
    ## Corrupt the response.
    CORRUPT = 3
    ## Drop the response and close the client (TCP-family).
    DISCONNECT = 4
    ## Reset the device state after the response.
    RESTART = 5
    ## Turn successful FC 02/04 reads into exception 04.
    SENSOR_DISCONNECT = 6


class Status(enum.IntEnum):
    """@brief Why a server could not be created; see MogbusError.status.
    """
    ## No error.
    OK = 0
    ## Bad options, no devices, or a limit reached.
    INVALID = 1
    ## The serial device or PTY could not be opened.
    SERIAL = 2
    ## The port could not be bound.
    LISTEN = 3


class LogLevel(enum.IntEnum):
    """@brief Verbosity of the library's log; see set_log_level().
    """
    ## Everything.
    DEBUG = 0
    ## Start, stop and client events.
    INFO = 1
    ## Warnings and errors only.
    WARN = 2
    ## Nothing.
    OFF = 3


class LogFormat(enum.IntEnum):
    """@brief Layout of the library's log lines; see set_log_format().
    """
    ## Human-readable lines.
    TEXT = 0
    ## One JSON object per line.
    JSON = 1


_NETWORK_MODES = (Mode.TCP, Mode.RTU_TCP, Mode.ASCII_TCP, Mode.UDP, Mode.RTU_UDP, Mode.ASCII_UDP)


def _coerce(cls, value, what):
    """Accepts a member, its int value or a name such as "rtu-tcp", "f32" or "high-first"."""
    if isinstance(value, cls):
        return value
    if isinstance(value, str):
        key = value.strip().upper().replace("-", "_").replace(".", "_")
        if key[:1].isdigit():
            key = "V" + key
        try:
            return cls[key]
        except KeyError:
            pass
    elif isinstance(value, int) and not isinstance(value, bool):
        try:
            return cls(value)
        except ValueError:
            pass
    names = ", ".join(m.name.lower().replace("_", "-") for m in cls)
    raise MogbusError(f"invalid {what} {value!r} (expected one of: {names})")


def _u(value, what, limit):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < limit:
        raise MogbusError(f"{what} must be an integer from 0 to {limit - 1}, got {value!r}")
    return value


def _text(value: Optional[str]) -> Optional[bytes]:
    return None if value is None else os.fsencode(value)


def _decode(value: Optional[bytes]) -> Optional[str]:
    return None if value is None else os.fsdecode(value)


@dataclass
class Access:
    """@brief A Modbus client access handed to a Device access hook.

    Check `target` first: a hook written for tables is also called for file records, FIFO queues and
    CANopen objects.
    @param kind READ or WRITE.
    @param space The table (for other targets, the table of the function code, usually HOLDING).
    @param address First address. FILE_RECORD: the first record. FIFO: the queue address. CANOPEN: the byte offset.
    @param count Registers or bits; FILE_RECORD: the records; CANOPEN: the bytes.
    @param values For writes of a table or a file record, the raw values about to be stored (bits as 0/1); for reads, None.
    @param function_code The Modbus function code of the request.
    @param target What is touched, an AccessTarget (default TABLE).
    @param file FILE_RECORD: the file number.
    @param node CANOPEN: the node id.
    @param index CANOPEN: the object index.
    @param subindex CANOPEN: the object subindex.
    @param data CANOPEN writes: the bytes about to be stored; otherwise None.
    """
    kind: AccessKind
    space: Space
    address: int
    count: int  # registers or bits
    values: Optional[List[int]]  # TABLE/FILE_RECORD writes: the raw values about to be stored (bits as 0/1); reads: None
    function_code: int
    # What is touched. For FILE_RECORD, `address` is the first record and `count` the records; for
    # FIFO, `address` is the queue address; for CANOPEN, `address` is the byte offset and `count` the bytes.
    target: AccessTarget = AccessTarget.TABLE
    file: int = 0  # FILE_RECORD: file number
    node: int = 0  # CANOPEN
    index: int = 0  # CANOPEN
    subindex: int = 0  # CANOPEN
    data: Optional[bytes] = None  # CANOPEN writes: the bytes about to be stored; reads: None


@dataclass
class Ramp:
    """@brief A point that moves over time: from `from_` to `to` by `step` every `every_ms`.

    Anywhere a Ramp is accepted, a dict works with the key `"from"` in place of `from_`, for
    example `{"from": 0, "to": 100, "step": 10, "every_ms": 1000, "mode": "bounce"}`.
    @param from_ Start value.
    @param to End value.
    @param step Amount added at every step.
    @param every_ms Milliseconds between two steps.
    @param mode A RampMode or its name: `"wrap"` (default), `"bounce"` or `"hold"`.
    """
    from_: float
    to: float
    step: float
    every_ms: int
    mode: Union[str, int, RampMode] = "wrap"

    @classmethod
    def of(cls, value: Union["Ramp", Dict[str, Any]]) -> "Ramp":
        """@brief Returns `value` as a Ramp.
        @param value A Ramp, or a dict with the keys of its fields (`"from"` for `from_`).
        @return The Ramp.
        @throws MogbusError If the dict has missing or unknown keys, or the value is neither.
        """
        if isinstance(value, Ramp):
            return value
        if not isinstance(value, dict):
            raise MogbusError(f"ramp must be a Ramp or a dict, got {value!r}")
        data = dict(value)
        if "from" in data:
            data["from_"] = data.pop("from")
        try:
            return cls(**data)
        except TypeError as exc:
            raise MogbusError(f"invalid ramp: {exc}") from None


@dataclass
class FaultInfo:
    """@brief One fault of a device as the config defined it; see DeviceInfo.faults.
    @param scheduled True when the fault fires on a schedule rather than by percent.
    @param percent Percent of requests it affects (percent faults).
    @param delay_ms Delay of a `delay` fault, in milliseconds.
    @param exception_code Code of an `exception` fault.
    """
    scheduled: bool
    percent: int
    delay_ms: int
    exception_code: int


@dataclass
class CustomFunctionCode:
    """@brief A user-defined function code of a device (config `(function-code ...)`).
    @param code The function code, 1-127.
    @param name Its name in the config.
    """
    code: int
    name: str


@dataclass
class DeviceInfo:
    """@brief What Device.info() returns.
    @param name Device name.
    @param unit_id Unit id.
    @param any_unit True when the device also answers unit ids no other device has.
    @param point_counts Number of points per table, keyed `"coil"`, `"discrete"`, `"input"`, `"holding"`.
    @param faults The configured faults only, keyed by kind name (for example `"delay"`, `"sensor-disconnect"`).
    @param custom_function_codes The device's user-defined function codes.
    """
    name: str
    unit_id: int
    any_unit: bool
    point_counts: Dict[str, int]
    faults: Dict[str, FaultInfo]
    custom_function_codes: List[CustomFunctionCode]


@dataclass
class ServerOptions:
    """@brief Server settings, with the defaults of a config file: TCP on `127.0.0.1:1502`.

    The fields are those of `mogbus_server_options_t` in snake_case. Fields a mode does not use are
    ignored. Strings are encoded for the duration of the C call and copied by the library. A field
    holding an enum accepts the member, its name or its int. See the configuration reference for the
    meaning of each in a config file.
    @param mode Transport: a Mode or its name (`"tcp"`, `"rtu-tcp"`, `"ascii-tcp"`, `"udp"`, `"rtu-udp"`, `"ascii-udp"`, the PTY, device and fd modes).
    @param bind_address TCP-family and UDP modes: address to bind; `"0.0.0.0"` opts in to remote access.
    @param port TCP-family and UDP modes; 0 asks serve() for a free loopback port; 802 by convention with TLS.
    @param path `*_DEVICE` modes: the serial device.
    @param fd `*_FD` modes: an open byte-stream fd you own; the library never closes it.
    @param seed Seed of the fault dice; 0 is a fixed default.
    @param idle_timeout_ms TCP-family modes: drop idle clients after this; 0 never.
    @param frame_gap_ms RTU modes: inter-frame gap override; 0 derives it from the baud rate.
    @param baud RTU timing and serial speed; 0 unknown.
    @param strict_t15 RTU byte streams: discard frames with a t1.5 gap.
    @param broadcast_execute RTU modes: run unit-0 requests (still no reply).
    @param data_bits Serial: 7 or 8 (RTU always uses 8).
    @param parity Serial: a Parity or its name.
    @param stop_bits Serial: 1 or 2.
    @param tls_enabled TCP mode only; needs a library built with TLS.
    @param tls_handshake_timeout_ms Close unfinished TLS handshakes after this many milliseconds.
    @param tls_certificate Server certificate chain, PEM file.
    @param tls_private_key Server private key, PEM file.
    @param tls_ca_certificate CA used to verify clients; None does not verify them.
    @param tls_require_client_certificate Reject clients without a certificate.
    @param tls_min_version A TlsVersion or its name (`"1.2"`, `"1.3"`).
    @param tls_ciphers_1_2 TLS 1.2 cipher list; None is the OpenSSL default.
    @param tls_ciphersuites_1_3 TLS 1.3 cipher suites; None is the OpenSSL default.
    @param http_bind_address Address of the HTTP endpoint (`/healthz`, `/readyz`, `/metrics`); None turns it off.
    @param http_port Port of that endpoint; used when `http_bind_address` is set.
    @param http_control Serve the runtime control API under `/api/` (needs the endpoint; no authentication, keep it on loopback).
    @param http_ui Serve the web UI on `/` (needs the endpoint; implies `http_control`).
    @param http_allowed_hosts Extra Host names `/api/` and the UI accept (loopback and the bind address always pass): a comma-separated string or a list of at most 8 names.
    """
    mode: Union[str, int, Mode] = Mode.TCP
    bind_address: Optional[str] = "127.0.0.1"
    port: int = 1502
    path: Optional[str] = None
    fd: int = -1
    seed: int = 0
    idle_timeout_ms: int = 0
    frame_gap_ms: int = 0
    baud: int = 0
    strict_t15: bool = False
    broadcast_execute: bool = False
    data_bits: int = 8
    parity: Union[str, int, Parity] = Parity.NONE
    stop_bits: int = 1
    tls_enabled: bool = False
    tls_handshake_timeout_ms: int = 10000
    tls_certificate: Optional[str] = None
    tls_private_key: Optional[str] = None
    tls_ca_certificate: Optional[str] = None
    tls_require_client_certificate: bool = True
    tls_min_version: Union[str, int, TlsVersion] = TlsVersion.V1_2
    tls_ciphers_1_2: Optional[str] = None
    tls_ciphersuites_1_3: Optional[str] = None
    http_bind_address: Optional[str] = None
    http_port: int = 9102
    http_control: bool = False
    http_ui: bool = False
    http_allowed_hosts: Union[None, str, Sequence[str]] = None

    def _to_c(self):
        """Returns (struct, keepalive). The struct points into the bytes objects in keepalive."""
        c = _lib.ServerOptionsC()
        keep = []
        for f in dataclasses.fields(self):
            value = getattr(self, f.name)
            if f.name == "mode":
                value = int(_coerce(Mode, value, "mode"))
            elif f.name == "parity":
                value = int(_coerce(Parity, value, "parity"))
            elif f.name == "tls_min_version":
                value = int(_coerce(TlsVersion, value, "tls_min_version"))
            elif f.name == "http_allowed_hosts":
                if isinstance(value, (list, tuple)):
                    value = ",".join(value)
                if value is not None and not isinstance(value, str):
                    raise MogbusError(f"http_allowed_hosts must be a string, a list of strings or None, got {value!r}")
                value = _text(value)
                keep.append(value)
            elif f.name in ("bind_address", "path", "tls_certificate", "tls_private_key",
                            "tls_ca_certificate", "tls_ciphers_1_2", "tls_ciphersuites_1_3",
                            "http_bind_address"):
                if value is not None and not isinstance(value, (str, os.PathLike)):
                    raise MogbusError(f"{f.name} must be a string or None, got {value!r}")
                value = _text(None if value is None else os.fspath(value))
                keep.append(value)
            elif f.name in ("port", "http_port"):
                value = _u(value, f.name, 65536)
            elif f.name == "fd":
                if not isinstance(value, int) or isinstance(value, bool):
                    raise MogbusError(f"fd must be an integer, got {value!r}")
            elif f.name in ("strict_t15", "broadcast_execute", "tls_enabled",
                            "tls_require_client_certificate", "http_control", "http_ui"):
                value = bool(value)
            else:
                value = _u(value, f.name, 1 << 32)
            setattr(c, f.name, value)
        return c, keep

    @classmethod
    def _from_c(cls, c) -> "ServerOptions":
        kwargs = {}
        for f in dataclasses.fields(cls):
            value = getattr(c, f.name)
            if f.name == "mode":
                value = Mode(value)
            elif f.name == "parity":
                value = Parity(value)
            elif f.name == "tls_min_version":
                value = TlsVersion(value)
            elif isinstance(value, bytes):
                value = os.fsdecode(value)
            kwargs[f.name] = value
        return cls(**kwargs)


class RtuTiming(NamedTuple):
    """@brief The RTU timing a server would use; see rtu_timing().
    @param t35_us The Modbus t3.5 in microseconds; 0 when the baud rate is 0.
    @param gap_ms The inter-frame gap in milliseconds.
    """
    t35_us: int
    gap_ms: int


def _err_buf():
    return ctypes.create_string_buffer(ERR_SIZE)


def _msg(buf, default):
    return buf.value.decode(errors="replace") or default


def _locked(method):
    @functools.wraps(method)
    def wrapper(self, *args, **kwargs):
        with self._ctx._lock:
            return method(self, *args, **kwargs)
    return wrapper


class Device:
    """@brief A simulated device: points in four tables, and the host side of the process.

    Obtain it from Context.add_device(), Context.find_device() or Context.devices; it is valid
    until its Context is closed. Two kinds of write exist. write() is what a client write does: it also
    writes `read_only` points and fails on a point the host owns. set_value() is the host side: it
    makes the point host-owned (a ramp or model stops, a client write no longer changes it) and
    ignores `read_only`. Neither reaches the access hook, which sees only client requests.
    """

    def __init__(self, ctx: "Context", handle: int):
        self._ctx = ctx
        self._h = handle
        self._access_ref = None  # the C callback object, alive while the hook is installed
        self._written_ref = None  # same for the post-write hook
        self._function_refs = []  # C callback objects of add_function_code(), alive as long as the device

    def _live(self):
        self._ctx._check_open()
        return self._h

    def __repr__(self):
        return f"<Device unit={self.unit_id}>"

    @property
    @_locked
    def unit_id(self) -> int:
        """@brief The unit id of the device.
        """
        return self.info().unit_id

    @property
    @_locked
    def name(self) -> str:
        """@brief The device name; empty when none was given.
        """
        return self.info().name

    @_locked
    def add_point(self, space, address, type="u16", value=0, word_order="high-first", ramp=None,
                  byte_order="big", length=0, text=None, read_only=False, clock=None,
                  clock_utc=False):
        """@brief Adds a point to a table.

        A point is static (`value`), a ramp (`ramp`) or a clock (`clock`); the three are exclusive. A
        multi-register type occupies consecutive addresses; an address that is already owned is rejected.
        `u64`/`s64` values are stored as floats here, so integers above 2^53 round (read() is exact).
        `byte_order="little"` swaps the two bytes inside every register of the point.
        @param space The table: a Space or its name.
        @param address First register or bit address, 0-65535.
        @param type A ValueType or its name; `"bool"` for coils and discrete inputs.
        @param value Initial value of a static point.
        @param word_order `"high-first"` (default) or `"low-first"` for 32 and 64-bit types.
        @param ramp A Ramp or dict: the point moves by itself; excludes `clock`.
        @param byte_order `"big"` (default) or `"little"`.
        @param length For type `"string"`: the number of registers, 1-123.
        @param text For type `"string"`: the initial text (Latin-1).
        @param read_only Coils and holding registers: Modbus writes fail with exception 02. Device.write() and set_value() still change the point.
        @param clock `"year"`, `"month"`, `"day"`, `"weekday"`, `"hour"`, `"minute"`, `"second"` or `"unix"`: an integer register that shows that field of the wall clock (`"unix"` needs 32 bits and is always UTC); excludes `ramp`.
        @param clock_utc Show the clock field in UTC rather than local time.
        @throws MogbusError If the point is invalid, overlaps another or does not fit the table.
        """
        p = _lib.PointC()
        p.space = int(_coerce(Space, space, "space"))
        p.address = _u(address, "address", 65536)
        p.type = int(_coerce(ValueType, type, "type"))
        p.word_order = int(_coerce(WordOrder, word_order, "word_order"))
        p.byte_order = int(_coerce(ByteOrder, byte_order, "byte_order"))
        p.length = _u(length, "length", 1 << 32)
        p.read_only = bool(read_only)
        if text is not None:
            if p.type != ValueType.STRING:
                raise MogbusError("text only applies to type \"string\"")
            p.text = text.encode("latin-1")
        if clock is not None:
            if ramp is not None:
                raise MogbusError("a point cannot have both a ramp and a clock")
            p.kind = 2
            p.clock_field = int(_coerce(ClockField, clock, "clock field"))
            p.clock_utc = bool(clock_utc)
        elif ramp is None:
            p.kind = 0
            p.value = float(value)
        else:
            r = Ramp.of(ramp)
            p.kind = 1
            p.ramp.from_ = float(r.from_)
            p.ramp.to = float(r.to)
            p.ramp.step = float(r.step)
            p.ramp.every_ms = _u(r.every_ms, "ramp every_ms", 1 << 32)
            p.ramp.mode = int(_coerce(RampMode, r.mode, "ramp mode"))
        err = _err_buf()
        if not lib.mogbus_device_add_point(self._live(), ctypes.byref(p), err, ERR_SIZE):
            raise MogbusError(_msg(err, "cannot add point"))

    @_locked
    def read(self, space, address, count=1) -> List[int]:
        """@brief Reads raw register (or bit) values the way a client read would, without the access hook.
        @param space The table.
        @param address First address.
        @param count Number of values; 0 returns an empty list.
        @return The values as ints (bits as 0/1).
        @throws MogbusError If an address is undefined or out of range.
        """
        space = int(_coerce(Space, space, "space"))
        address = _u(address, "address", 65536)
        count = _u(count, "count", 65537)
        if count == 0:
            return []
        out = (ctypes.c_uint16 * count)()
        if not lib.mogbus_device_read(self._live(), space, address, count, out):
            raise MogbusError(f"cannot read {count} value(s) at {Space(space).name.lower()} "
                              f"{address}: an address is undefined or out of range")
        return list(out)

    @_locked
    def write(self, space, address, values: Union[int, Sequence[int]]) -> None:
        """@brief Writes raw values the way a client write would, without the access hook.

        It also writes `read_only` points. All or nothing.
        @param space The table; clients can only write coils and holding registers, the host may also fill the others.
        @param address First address.
        @param values One value or a sequence of values.
        @throws MogbusError If an address is undefined or out of range, a value does not fit, or the point is driven by a machine or claimed by set_value().
        """
        if isinstance(values, int):
            values = [values]
        space = int(_coerce(Space, space, "space"))
        address = _u(address, "address", 65536)
        values = [_u(v, "register value", 65536) for v in values]
        if not values:
            return
        arr = (ctypes.c_uint16 * len(values))(*values)
        if not lib.mogbus_device_write(self._live(), space, address, len(values), arr):
            raise MogbusError(f"cannot write {len(values)} value(s) at {Space(space).name.lower()} "
                              f"{address}: an address is undefined or out of range, a value does "
                              "not fit, or the point is driven by a machine or the host")

    @_locked
    def get_value(self, space, address) -> float:
        """@brief The decoded value of the point that starts at `address`.
        @param space The table.
        @param address First address of the point.
        @return The value as a float (bits 0.0 or 1.0).
        @throws MogbusError If no point starts there, or it is a string (use get_string()).
        """
        space = int(_coerce(Space, space, "space"))
        address = _u(address, "address", 65536)
        out = ctypes.c_double()
        if not lib.mogbus_device_get_value(self._live(), space, address, ctypes.byref(out)):
            raise MogbusError(f"no point starts at {Space(space).name.lower()} {address}")
        return out.value

    @_locked
    def set_value(self, space, address, value: float) -> None:
        """@brief Sets the decoded value of the point that starts at `address` from the host.

        The point becomes host-owned: a ramp or model stops moving it and a later client write does not
        change it. This is how a test or simulation drives a process value. `read_only` is ignored.
        @param space The table.
        @param address First address of the point.
        @param value The new value.
        @throws MogbusError If no point starts there, the value does not fit the type, or it is a string (use set_string()).
        """
        space = int(_coerce(Space, space, "space"))
        address = _u(address, "address", 65536)
        if not lib.mogbus_device_set_value(self._live(), space, address, float(value)):
            raise MogbusError(f"no point starts at {Space(space).name.lower()} {address}")

    @_locked
    def set_any_unit(self, on: bool = True) -> None:
        """@brief Makes this device also answer every unit id no other device has.

        RTU and ASCII keep 0 and 248-255 unanswered.
        @param on True to turn it on, False to turn it off.
        @throws MogbusError If another device of the context already has it.
        """
        if not lib.mogbus_device_set_any_unit(self._live(), bool(on)):
            raise MogbusError("another device of the context already answers any unit id")

    @_locked
    def set_access_hook(self, hook: Optional[Callable[[Access], Optional[int]]]) -> None:
        """@brief Runs `hook(access)` on every Modbus client read or write of this device; None removes it.

        The hook sees a read after the range validates and before the table is read, and a write after
        the range is defined and writable and before anything is written. Besides the tables it runs for
        file records (FC 14, 15), FIFO queues (FC 18) and CANopen objects (FC 2B/0D); check Access.target.
        Diagnostics and identification are not covered, and host calls such as set_value() and write()
        never reach it.

        Return None (or 0) to let the request go on, or a Modbus exception code (1-255) to answer it
        instead. A vetoed write changes nothing; a CANopen veto is answered as the extended failure
        `CE 0x08000020` whatever code the hook returns. The hook runs on the thread that steps the server
        with the context lock held. The lock is reentrant, so the hook may call the methods of this
        device, for example set_value() to refresh a value just before a read. An exception raised by the
        hook is printed with its traceback and answered with exception 04.

        ```python
        def hook(access):
            if access.kind == mogbus.AccessKind.READ:
                dev.set_value("holding", 0, read_sensor())   # refresh just before the client reads
            elif access.values[0] > 1000:
                return 3                                      # illegal data value

        dev.set_access_hook(hook)
        ```
        @param hook A callable taking an Access and returning None or an exception code; None removes the hook.
        @throws MogbusError If the hook cannot be installed.
        """
        handle = self._live()
        if hook is None:
            if not lib.mogbus_device_set_access_hook(handle, ctypes.cast(None, _lib.ACCESS_FN), None):
                raise MogbusError("cannot remove the access hook")
            self._access_ref = None
            return

        def trampoline(_user, _dev, access):
            try:
                a = access.contents
                values = None
                if a.kind == AccessKind.WRITE and a.values:
                    values = [a.values[i] for i in range(a.count)]
                data = ctypes.string_at(a.data, a.data_len) if a.data else None
                code = hook(Access(AccessKind(a.kind), Space(a.space), a.address, a.count, values,
                                   a.function_code, AccessTarget(a.target), a.file, a.node, a.index,
                                   a.subindex, data))
                code = 0 if code is None else int(code)
                if not 0 <= code <= 255:
                    raise ValueError(f"exception code {code} is not in 0..255")
                return code
            except BaseException:  # an exception must not unwind into C
                traceback.print_exc()
                return 4

        ref = _lib.ACCESS_FN(trampoline)
        if not lib.mogbus_device_set_access_hook(handle, ref, None):
            raise MogbusError("cannot install the access hook (out of memory)")
        self._access_ref = ref

    @_locked
    def set_written_hook(self, hook: Optional[Callable[[Access], None]]) -> None:
        """@brief Runs `hook(access)` after every Modbus client write of this device was applied; None removes it.

        This is the equivalent of pymodbus `setValues`: react to a client write (a mode change, a command
        register) instead of polling for it. The hook runs once per written range, after the write is
        applied, for FC 05, 06, 15 (0x0F), 16 (0x10), 22 (0x16, the masked result), 23 (0x17, the write half,
        before the read half is answered) and, once per sub-request, for FC 21 (0x15) file records
        (Access.target is FILE_RECORD). Access.values holds the stored registers (bits as 0/1). It is not
        called for a write the access hook vetoed, for a request answered with an exception, or for host
        calls such as set_value() and write(). Faults do not change request processing, so it also runs when
        the response is then dropped or corrupted.

        The hook runs on the thread that steps the server with the context lock held. The lock is
        reentrant, so the hook may call the methods of this device, for example set_value() to update a
        status register. An exception raised by the hook is printed with its traceback and otherwise
        ignored: the write stays applied and the response is unchanged.

        ```python
        def written(access):
            if access.space == mogbus.Space.HOLDING and access.address == 0:
                dev.set_value("holding", 1, access.values[0])   # mirror the command into a status register

        dev.set_written_hook(written)
        ```
        @param hook A callable taking an Access and returning None; None removes the hook.
        @throws MogbusError If the hook cannot be installed.
        """
        handle = self._live()
        if hook is None:
            if not lib.mogbus_device_set_written_hook(handle, ctypes.cast(None, _lib.WRITTEN_FN), None):
                raise MogbusError("cannot remove the written hook")
            self._written_ref = None
            return

        def trampoline(_user, _dev, access):
            try:
                a = access.contents
                values = [a.values[i] for i in range(a.count)] if a.values else None
                hook(Access(AccessKind(a.kind), Space(a.space), a.address, a.count, values,
                            a.function_code, AccessTarget(a.target), a.file, a.node, a.index,
                            a.subindex, None))
            except BaseException:  # an exception must not unwind into C
                traceback.print_exc()

        ref = _lib.WRITTEN_FN(trampoline)
        if not lib.mogbus_device_set_written_hook(handle, ref, None):
            raise MogbusError("cannot install the written hook (out of memory)")
        self._written_ref = ref

    def add_function_code(self, code: int, callback: Callable[[bytes], Union[bytes, int, None]]) -> None:
        """@brief Answers the user-defined function code `code` by calling `callback(request)`.

        Like `(function-code CODE ...)` in a config, with code in place of the cases: unit routing,
        faults, listen-only, the FC 08 counters, metrics and the access log treat the request as any
        other. `request` is the bytes after the function code. The callback returns bytes (at most 252)
        to answer with that reply data, an int (1-255) to answer that Modbus exception, or None to send no
        response. Anything else, a reply that is too long, or an exception raised by the callback is
        answered with exception 04 (an exception is also printed with its traceback). With the server's
        `broadcast-execute` the callback runs for a broadcast and its reply is dropped.

        It runs on the thread that steps the server with the context lock held. The lock is reentrant,
        so the callback may call the methods of this device, for example get_value() to build a reply from
        live values. A code cannot be removed or redefined; it counts toward the limit of 8 per device and
        appears in info() with an empty name.

        ```python
        def status(request):
            if request[:1] != b"\x01":
                return 3                                  # illegal data value
            return struct.pack(">H", int(dev.get_value("holding", 0)))

        dev.add_function_code(0x41, status)
        ```
        @param code Function code, 1-127 and not one mogbus implements.
        @param callback A callable taking the request data and returning bytes, an exception code or None.
        @throws MogbusError If the code is invalid, built in, already defined, over the limit or out of memory.
        """
        code = _u(code, "function code", 128)

        def trampoline(_user, _dev, _unit, _code, data, length, reply, reply_cap, reply_len):
            try:
                result = callback(ctypes.string_at(data, length) if length else b"")
                if result is None:
                    return -1
                if isinstance(result, int) and not isinstance(result, bool):
                    if not 1 <= result <= 255:
                        raise ValueError(f"exception code {result} is not in 1..255")
                    return result
                result = bytes(result)
                if len(result) > reply_cap:
                    raise ValueError(f"reply of {len(result)} bytes is longer than {reply_cap}")
                ctypes.memmove(reply, result, len(result))
                reply_len[0] = len(result)
                return 0
            except BaseException:  # an exception must not unwind into C
                traceback.print_exc()
                return 4

        ref = _lib.FUNCTION_FN(trampoline)
        if not lib.mogbus_device_add_function_code(self._live(), code, ref, None):
            raise MogbusError(f"cannot add function code 0x{code:02X} (not 1-127, built in, already defined, "
                              "or the device has 8)")
        self._function_refs.append(ref)

    @_locked
    def add_file_record(self, file, record, values: Sequence[int]) -> None:
        """@brief Adds file records `record` onward of file `file` (FC 14/15) with these initial register values.
        @param file File number.
        @param record First record number.
        @param values Initial register values, 1-126.
        @throws MogbusError If the records are invalid or overlap existing ones.
        """
        file = _u(file, "file", 65536)
        record = _u(record, "record", 65536)
        values = [_u(v, "register value", 65536) for v in values]
        arr = (ctypes.c_uint16 * max(len(values), 1))(*values)
        err = _err_buf()
        if not lib.mogbus_device_add_file_record(self._live(), file, record, len(values), arr, err, ERR_SIZE):
            raise MogbusError(_msg(err, "cannot add file record"))

    @_locked
    def read_file_records(self, file, record, count=1) -> List[int]:
        """@brief The values of `count` consecutive file records (host side: no access hook).
        @param file File number.
        @param record First record.
        @param count Number of records.
        @return The values.
        @throws MogbusError If a record is undefined.
        """
        file = _u(file, "file", 65536)
        record = _u(record, "record", 65536)
        count = _u(count, "count", 65536)
        out = (ctypes.c_uint16 * max(count, 1))()
        if not lib.mogbus_device_read_file_records(self._live(), file, record, count, out):
            raise MogbusError(f"cannot read {count} record(s) at file {file} record {record}: "
                              "a record is undefined")
        return list(out)[:count]

    @_locked
    def write_file_records(self, file, record, values: Union[int, Sequence[int]]) -> None:
        """@brief Writes consecutive file records (host side: no access hook); all or nothing.
        @param file File number.
        @param record First record.
        @param values One value or a sequence of values.
        @throws MogbusError If a record is undefined.
        """
        if isinstance(values, int):
            values = [values]
        file = _u(file, "file", 65536)
        record = _u(record, "record", 65536)
        values = [_u(v, "register value", 65536) for v in values]
        arr = (ctypes.c_uint16 * max(len(values), 1))(*values)
        if not lib.mogbus_device_write_file_records(self._live(), file, record, len(values), arr):
            raise MogbusError(f"cannot write {len(values)} record(s) at file {file} record {record}: "
                              "a record is undefined")

    @_locked
    def add_fifo(self, address, values: Sequence[int] = ()) -> None:
        """@brief Adds a FIFO queue read by FC 18 at `address`.
        @param address Queue address.
        @param values Up to 31 initial values (the storage always has room for 31).
        @throws MogbusError If the address is already used or there are too many values.
        """
        address = _u(address, "address", 65536)
        values = [_u(v, "register value", 65536) for v in values]
        arr = (ctypes.c_uint16 * max(len(values), 1))(*values)
        err = _err_buf()
        if not lib.mogbus_device_add_fifo(self._live(), address, len(values), arr, err, ERR_SIZE):
            raise MogbusError(_msg(err, "cannot add FIFO"))

    @_locked
    def set_fifo(self, address, values: Sequence[int] = ()) -> None:
        """@brief Replaces the contents of the FIFO queue at `address` (host side).
        @param address Queue address.
        @param values Up to 31 values.
        @throws MogbusError If no FIFO is defined there, or there are more than 31 values.
        """
        address = _u(address, "address", 65536)
        values = [_u(v, "register value", 65536) for v in values]
        arr = (ctypes.c_uint16 * max(len(values), 1))(*values)
        if not lib.mogbus_device_set_fifo(self._live(), address, len(values), arr):
            raise MogbusError(f"no FIFO is defined at {address}, or more than 31 values")

    @_locked
    def add_canopen_object(self, node, index, subindex, data: bytes) -> None:
        """@brief Adds a CANopen object (FC 2B/0D) of fixed length `len(data)`.
        @param node Node id, 1-127.
        @param index Object index.
        @param subindex Object subindex.
        @param data Initial bytes, 1-238; the object keeps this length.
        @throws MogbusError If the object is invalid or exists.
        """
        node = _u(node, "node", 256)
        index = _u(index, "index", 65536)
        subindex = _u(subindex, "subindex", 256)
        data = bytes(data)
        arr = (ctypes.c_uint8 * max(len(data), 1)).from_buffer_copy(data or b"\0")
        err = _err_buf()
        if not lib.mogbus_device_add_canopen_object(self._live(), node, index, subindex, len(data), arr,
                                                    err, ERR_SIZE):
            raise MogbusError(_msg(err, "cannot add CANopen object"))

    @_locked
    def read_canopen(self, node, index, subindex, count, offset=0) -> bytes:
        """@brief `count` bytes at `offset` of a CANopen object (host side: no access hook).
        @param node Node id.
        @param index Object index.
        @param subindex Object subindex.
        @param count Number of bytes.
        @param offset First byte.
        @return The bytes.
        @throws MogbusError If the object does not exist or the range is outside it.
        """
        node = _u(node, "node", 256)
        index = _u(index, "index", 65536)
        subindex = _u(subindex, "subindex", 256)
        count = _u(count, "count", 65536)
        offset = _u(offset, "offset", 65536)
        out = (ctypes.c_uint8 * max(count, 1))()
        if not lib.mogbus_device_read_canopen(self._live(), node, index, subindex, offset, count, out):
            raise MogbusError("no such CANopen object, or the range is outside it")
        return bytes(out)[:count]

    @_locked
    def write_canopen(self, node, index, subindex, data: bytes, offset=0) -> None:
        """@brief Writes bytes at `offset` of a CANopen object; its length never changes (host side).
        @param node Node id.
        @param index Object index.
        @param subindex Object subindex.
        @param data Bytes to store.
        @param offset First byte.
        @throws MogbusError If the object does not exist or the range is outside it.
        """
        node = _u(node, "node", 256)
        index = _u(index, "index", 65536)
        subindex = _u(subindex, "subindex", 256)
        offset = _u(offset, "offset", 65536)
        data = bytes(data)
        arr = (ctypes.c_uint8 * max(len(data), 1)).from_buffer_copy(data or b"\0")
        if not lib.mogbus_device_write_canopen(self._live(), node, index, subindex, offset, len(data), arr):
            raise MogbusError("no such CANopen object, or the range is outside it")

    @_locked
    def get_string(self, space, address) -> str:
        """@brief The text of the string point at `address`, up to its first NUL byte.
        @param space The table (holding or input).
        @param address First address of the point.
        @return The text; bytes are decoded as Latin-1.
        @throws MogbusError If no string point starts there.
        """
        space = int(_coerce(Space, space, "space"))
        address = _u(address, "address", 65536)
        buf = ctypes.create_string_buffer(2 * 123 + 1)
        if not lib.mogbus_device_get_string(self._live(), space, address, buf, len(buf)):
            raise MogbusError(f"no string point starts at {Space(space).name.lower()} {address}")
        return buf.value.decode("latin-1")

    @_locked
    def set_string(self, space, address, text: str) -> None:
        """@brief Replaces the text of a string point, NUL padded. Clients can still write it unless it is `read_only`.
        @param space The table.
        @param address First address of the point.
        @param text The new text (Latin-1).
        @throws MogbusError If no string point starts there, or the text is longer than its registers.
        """
        space = int(_coerce(Space, space, "space"))
        address = _u(address, "address", 65536)
        if not lib.mogbus_device_set_string(self._live(), space, address, text.encode("latin-1")):
            raise MogbusError(f"no string point starts at {Space(space).name.lower()} {address}, "
                              "or the text is longer than its registers")

    @_locked
    def info(self) -> DeviceInfo:
        """@brief Describes the device: name, unit id, point counts, configured faults and custom function codes.
        @return A DeviceInfo.
        """
        c = _lib.DeviceInfoC()
        if not lib.mogbus_device_info(self._live(), ctypes.byref(c)):
            raise MogbusError("cannot read device info")
        faults = {
            FaultKind(i).name.lower().replace("_", "-"):
                FaultInfo(f.scheduled, f.percent, f.delay_ms, f.exception_code)
            for i, f in enumerate(c.faults) if f.present
        }
        fcs = [CustomFunctionCode(c.custom_fcs[i].code, _decode(c.custom_fcs[i].name) or "")
               for i in range(min(c.custom_fc_count, 8))]
        return DeviceInfo(
            name=_decode(c.name) or "",
            unit_id=c.unit_id,
            any_unit=bool(c.any_unit),
            point_counts={Space(i).name.lower(): c.point_count[i] for i in range(4)},
            faults=faults,
            custom_function_codes=fcs)


class Server:
    """@brief A transport serving the devices of a Context.

    Create it with Context.serve() or Context.serve_config(). It does nothing until you call
    start() (background thread), run() (blocks) or step() (one poll iteration, for a loop of your
    own). Usable as a context manager that closes it. The attribute `last_step_result` holds the result of
    the last step of the background thread (0 continue, 1 done, 2 poll error), None before any.
    """

    ## Milliseconds the background thread waits in one step.
    STEP_MS = 10

    def __init__(self, ctx: "Context", handle: int):
        self._ctx = ctx
        self._h = handle
        self._thread: Optional[threading.Thread] = None
        self._stopping = False
        self.last_step_result: Optional[int] = None

    def _live(self):
        if not self._h:
            raise MogbusError("server is closed")
        return self._h

    @_locked
    def step(self, max_wait_ms: int = 0) -> int:
        """@brief Runs one poll iteration: accepts, reads, answers and writes whatever is ready.
        @param max_wait_ms Longest time to block: 0 does not block, -1 waits until the server's next deadline (at most 1 s).
        @return 0 to continue, 1 when done (stopped, or the byte-stream peer went away), 2 on a poll error.
        """
        return lib.mogbus_server_step(self._live(), int(max_wait_ms))

    def _loop(self):
        ctx = self._ctx
        while not self._stopping:
            with ctx._lock:
                if not self._h:
                    return 0
                result = lib.mogbus_server_step(self._h, self.STEP_MS)
            self.last_step_result = result
            if result != 0:
                return 1 if result == 2 else 0
            # A Python lock is not fair: give a waiting caller the chance to take it.
            time.sleep(0.0002)
        return 0

    def run(self, exclusive: bool = False) -> int:
        """@brief Steps until stopped; blocks the calling thread.

        By default the context lock is released between steps so other threads can use the devices.
        @param exclusive True calls the C `mogbus_server_run()`, which holds the lock for the whole run.
        @return 0 after a clean stop, 1 on a poll error.
        """
        if exclusive:
            with self._ctx._lock:
                return lib.mogbus_server_run(self._live())
        return self._loop()

    def start(self) -> "Server":
        """@brief Runs the server on a background thread; stop() or close() joins it.

        The thread calls step() under the context lock and releases it between steps, so a call from
        another thread waits for the current step (at most about 10 ms when the server is idle).
        @return The server, so `ctx.serve(...).start()` chains.
        @throws MogbusError If the server is closed or already started.
        """
        with self._ctx._lock:
            self._live()
            if self._thread is not None:
                raise MogbusError("server thread already started")
            self._stopping = False
            self._thread = threading.Thread(target=self._loop, name="mogbus-server", daemon=True)
            self._thread.start()
        return self

    def stop(self) -> None:
        """@brief Asks the server to stop and joins the background thread, if any. Takes no lock.
        """
        self._stopping = True
        h = self._h
        if h:
            lib.mogbus_server_stop(h)
        thread = self._thread
        if thread is not None and thread is not threading.current_thread():
            thread.join()
            self._thread = None

    def close(self) -> None:
        """@brief Stops, joins the thread and closes the transport. Safe to call twice.
        """
        self.stop()
        with self._ctx._lock:
            if self._h:
                lib.mogbus_server_close(self._h)
                self._h = None
                with contextlib.suppress(ValueError):
                    self._ctx._servers.remove(self)

    @property
    def closed(self) -> bool:
        """@brief True once close() ran.
        """
        return not self._h

    @property
    @_locked
    def pty_name(self) -> Optional[str]:
        """@brief The slave path of the PTY of an `rtu-pty` or `ascii-pty` server, else None.
        """
        return _decode(lib.mogbus_server_pty_name(self._live()))

    @_locked
    def metrics(self) -> str:
        """@brief Prometheus text of this server's counters (the same as `/metrics`).
        @return The text exposition.
        @throws MogbusError If it does not fit in 64 MiB.
        """
        size = 32 * 1024
        while size <= 64 * 1024 * 1024:
            buf = ctypes.create_string_buffer(size)
            n = lib.mogbus_server_metrics(self._live(), buf, size)
            if n:
                return buf.raw[:n].decode()
            size *= 2
        raise MogbusError("metrics do not fit in 64 MiB")

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class Context:
    """@brief Owns devices and servers; the root of the API.

    All methods of a context, its devices and its servers share one re-entrant lock, so they may be
    called from any thread. Usable as a context manager; `len(ctx)` is the number of devices and
    iterating it yields them. At most 16 devices and 8 servers per context.
    """

    def __init__(self):
        self._lock = threading.RLock()
        self._h = lib.mogbus_create()
        if not self._h:
            raise MogbusError("cannot create a context (memory cannot be mapped)")
        self._servers: List[Server] = []
        self._devices: Dict[int, Device] = {}

    def _check_open(self):
        if not self._h:
            raise MogbusError("context is closed")

    def _device(self, handle) -> Optional[Device]:
        if not handle:
            return None
        dev = self._devices.get(handle)
        if dev is None:
            dev = self._devices[handle] = Device(self, handle)
        return dev

    def close(self) -> None:
        """@brief Closes every server and releases all memory. Safe to call twice.

        After it, the context's devices and servers raise MogbusError("context is closed").
        """
        for server in list(self._servers):
            server.close()
        with self._lock:
            if self._h:
                lib.mogbus_destroy(self._h)
                self._h = None
                self._devices.clear()

    @property
    def closed(self) -> bool:
        """@brief True once close() ran.
        """
        return not self._h

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        with contextlib.suppress(Exception):
            self.close()

    def load_config(self, path) -> None:
        """@brief Loads a config file: its devices (and its `(server ...)` section, see config_server_options()).
        @param path Path to the `.scm` file.
        @throws MogbusError If the file cannot be read or is invalid; the message names the line.
        """
        with self._lock:
            self._check_open()
            err = _err_buf()
            if not lib.mogbus_load_config(self._h, _text(os.fspath(path)), err, ERR_SIZE):
                raise MogbusError(_msg(err, "cannot load config"))

    def add_device(self, unit_id: int, name: Optional[str] = None) -> Device:
        """@brief Adds an empty device.
        @param unit_id Unit id, 0-255 (RTU and ASCII answer 1-247).
        @param name Optional name, shown in logs and by Device.info().
        @return The Device.
        @throws MogbusError If the unit id is taken or the limit of 16 devices is reached.
        """
        with self._lock:
            self._check_open()
            unit_id = _u(unit_id, "unit_id", 256)
            err = _err_buf()
            handle = lib.mogbus_add_device(self._h, unit_id, _text(name), err, ERR_SIZE)
            if not handle:
                raise MogbusError(_msg(err, "cannot add device"))
            return self._device(handle)

    def find_device(self, unit_id: int) -> Optional[Device]:
        """@brief The device with this unit id.
        @param unit_id Unit id.
        @return The Device, or None.
        """
        with self._lock:
            self._check_open()
            return self._device(lib.mogbus_find_device(self._h, _u(unit_id, "unit_id", 256)))

    @property
    def devices(self) -> List[Device]:
        """@brief The devices of the context, in the order they were added.
        """
        with self._lock:
            self._check_open()
            return [self._device(lib.mogbus_device_at(self._h, i))
                    for i in range(lib.mogbus_device_count(self._h))]

    def __iter__(self) -> Iterator[Device]:
        return iter(self.devices)

    def __len__(self) -> int:
        with self._lock:
            self._check_open()
            return lib.mogbus_device_count(self._h)

    def config_server_options(self) -> Optional[ServerOptions]:
        """@brief The `(server ...)` section of the loaded config as ServerOptions.
        @return The options, or None when no config is loaded.
        """
        with self._lock:
            self._check_open()
            c = _lib.ServerOptionsC()
            if not lib.mogbus_config_server_options(self._h, ctypes.byref(c)):
                return None
            return ServerOptions._from_c(c)

    def check_server(self, options: Optional[ServerOptions] = None, **overrides) -> None:
        """@brief Validates options exactly as serve() would, without starting anything.
        @param options Server settings; default settings when None.
        @param overrides ServerOptions fields to override, as keyword arguments.
        @throws MogbusError If serve() would reject them.
        """
        options = _merge(options, overrides)
        with self._lock:
            self._check_open()
            c, keep = options._to_c()
            err = _err_buf()
            if not lib.mogbus_server_check(self._h, ctypes.byref(c), err, ERR_SIZE):
                raise MogbusError(_msg(err, "invalid server options"))
            del keep

    def serve(self, options: Optional[ServerOptions] = None, **overrides) -> Server:
        """@brief Creates a server for every device the context has now.

        Not started: call Server.start(), Server.run() or Server.step(). A device added later is not
        served by this server.
        @param options Server settings; default settings (TCP on 127.0.0.1:1502) when None.
        @param overrides ServerOptions fields to override, as keyword arguments.
        @return The Server.
        @throws MogbusError If the options are invalid, there are no devices or the transport cannot start (for example the port is in use); `status` tells which.
        """
        options = _merge(options, overrides)
        with self._lock:
            self._check_open()
            c, keep = options._to_c()
            err = _err_buf()
            status = ctypes.c_int(0)
            handle = lib.mogbus_server_create(self._h, ctypes.byref(c), err, ERR_SIZE,
                                              ctypes.byref(status))
            del keep
            return self._new_server(handle, err, status)

    def serve_config(self) -> Server:
        """@brief Creates a server from the `(server ...)` section of the loaded config.
        @return The Server, not started.
        @throws MogbusError As serve().
        """
        with self._lock:
            self._check_open()
            err = _err_buf()
            status = ctypes.c_int(0)
            handle = lib.mogbus_server_create_from_config(self._h, err, ERR_SIZE,
                                                          ctypes.byref(status))
            return self._new_server(handle, err, status)

    def _new_server(self, handle, err, status):
        if not handle:
            raise MogbusError(_msg(err, "cannot create server"), Status(status.value))
        server = Server(self, handle)
        self._servers.append(server)
        return server


def _merge(options: Optional[ServerOptions], overrides) -> ServerOptions:
    options = ServerOptions() if options is None else options
    if overrides:
        try:
            options = dataclasses.replace(options, **overrides)
        except TypeError as exc:
            raise MogbusError(f"invalid server option: {exc}") from None
    return options


def probe(options: ServerOptions, unit_id: int, timeout_ms: int = 2000) -> bool:
    """@brief Checks that a server answers: one FC 08 sub-function 0 echo to `unit_id`; see probe_or_raise().

    It blocks the caller and takes no lock, so it works while a started server runs on its thread.
    @param options Settings of the server to probe, for example Context.config_server_options().
    @param unit_id Unit to ask, not 0.
    @param timeout_ms Time to wait for the reply.
    @return True for the echo or a Modbus exception reply; False otherwise.
    """
    try:
        probe_or_raise(options, unit_id, timeout_ms)
    except MogbusError:
        return False
    return True


def probe_or_raise(options: ServerOptions, unit_id: int, timeout_ms: int = 2000) -> None:
    """@brief As probe(), but raises with the reason.

    Works over tcp, udp, rtu-tcp, rtu-udp, ascii-tcp and ascii-udp. A bind of `0.0.0.0` is reached
    through loopback; a TLS server is only connected to. A fault that drops or delays replies on the
    unit makes it fail.
    @param options Settings of the server to probe.
    @param unit_id Unit to ask, not 0.
    @param timeout_ms Time to wait for the reply.
    @throws MogbusError On a timeout, a refused connection, a malformed reply, a serial or fd mode, or unit 0.
    """
    c, keep = options._to_c()
    err = _err_buf()
    ok = lib.mogbus_probe(ctypes.byref(c), _u(unit_id, "unit_id", 256),
                          _u(timeout_ms, "timeout_ms", 1 << 32), err, ERR_SIZE)
    del keep
    if not ok:
        raise MogbusError(_msg(err, "probe failed"))


def rtu_timing(baud: int, frame_gap_ms: int = 0) -> RtuTiming:
    """@brief Computes the RTU timing a server would use.
    @param baud Baud rate; 0 unknown.
    @param frame_gap_ms Explicit inter-frame gap; 0 derives it from `baud` (20 ms when `baud` is 0).
    @return An RtuTiming.
    """
    t35 = ctypes.c_uint32()
    gap = ctypes.c_uint32()
    lib.mogbus_rtu_timing(_u(baud, "baud", 1 << 32), _u(frame_gap_ms, "frame_gap_ms", 1 << 32),
                          ctypes.byref(t35), ctypes.byref(gap))
    return RtuTiming(t35.value, gap.value)


def library_version() -> str:
    """@brief The version of the loaded library (`mogbus_version()`).
    @return The version string.
    """
    return _lib.LIBRARY_VERSION


_log_ref = None


def set_log_level(level) -> None:
    """@brief Sets the minimum level of the process-wide log.
    @param level A LogLevel or its name.
    """
    lib.mogbus_set_log_level(int(_coerce(LogLevel, level, "log level")))


def set_log_format(fmt) -> None:
    """@brief Sets the layout of the process-wide log lines.
    @param fmt A LogFormat or its name: `"text"` or `"json"`; with JSON, `message` of a log sink is a JSON object.
    """
    lib.mogbus_set_log_format(int(_coerce(LogFormat, fmt, "log format")))


def set_log_sink(callback: Optional[Callable[[LogLevel, str], None]]) -> None:
    """@brief Sends log lines to `callback(level, message)` instead of stderr; None restores stderr.

    Process-wide. The callback runs on the thread that logs, which is the server thread while a
    server is started, and with the context lock held: keep it short and do not block in it. An
    exception it raises is ignored.
    @param callback A callable taking a LogLevel and the message string, or None.
    """
    global _log_ref

    if callback is None:
        lib.mogbus_set_log_sink(ctypes.cast(None, _lib.LOG_FN), None)
        _log_ref = None
        return

    def trampoline(_user, level, message):
        try:
            callback(LogLevel(level), message.decode(errors="replace"))
        except Exception:  # an exception must not unwind into C
            pass

    ref = _lib.LOG_FN(trampoline)
    _log_ref = ref  # keep it alive for as long as C may call it
    lib.mogbus_set_log_sink(ref, None)


def _free_port(host="127.0.0.1", kind=socket.SOCK_STREAM) -> int:
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind((host, 0))
        return s.getsockname()[1]


@dataclass
class Served:
    """@brief What serve() yields.
    @param context The Context.
    @param server The started Server.
    @param port The port actually served (the HTTP endpoint port is separate).
    """
    context: Context
    server: Server
    port: int

    def device(self, unit_id: int) -> Device:
        """@brief The device with this unit id.
        @param unit_id Unit id.
        @return The Device.
        @throws MogbusError If there is none.
        """
        dev = self.context.find_device(unit_id)
        if dev is None:
            raise MogbusError(f"no device with unit id {unit_id}")
        return dev


@contextlib.contextmanager
def serve(config=None, config_text: Optional[str] = None, **options):
    """@brief Context manager for tests: loads a config, serves it from a background thread and cleans up on exit.

    The server uses the config's own `(server ...)` section; the keyword arguments you pass
    override its fields (`mode`, `port`, `bind_address`, any ServerOptions field). A config without
    a section gets the config defaults (TCP 1502), as in the binary. With no config at all the
    server is TCP on a free loopback port. In a network mode, `port=0` picks a free port (read it
    from Served.port). On exit it stops the thread and destroys the context.

    ```python
    with mogbus.serve(config="profiles/minimal.scm", port=0) as srv:
        srv.device(1).set_value("holding", 0, 1234)   # a client on srv.port now reads 1234
    ```
    @param config Path to a config file.
    @param config_text The config as text (written to a temporary file); not with `config`.
    @param options ServerOptions fields to override.
    @return A context manager yielding a Served.
    @throws MogbusError If both `config` and `config_text` are given, the config is invalid or the server cannot start.
    """
    if config is not None and config_text is not None:
        raise MogbusError("pass config or config_text, not both")
    ctx = Context()
    try:
        base = None
        tmp = None
        try:
            if config_text is not None:
                with tempfile.NamedTemporaryFile("w", suffix=".scm", delete=False) as f:
                    f.write(config_text)
                tmp = config = f.name
            if config is not None:
                ctx.load_config(config)
                base = ctx.config_server_options()
        finally:
            if tmp:
                os.unlink(tmp)
        if base is None:
            base = ServerOptions(port=0)
        opts = _merge(base, options)
        mode = _coerce(Mode, opts.mode, "mode")
        if mode in _NETWORK_MODES and not opts.port:
            kind = socket.SOCK_DGRAM if mode in (Mode.UDP, Mode.RTU_UDP, Mode.ASCII_UDP) \
                else socket.SOCK_STREAM
            opts.port = _free_port(opts.bind_address or "127.0.0.1", kind)
        server = ctx.serve(opts)
        server.start()
        yield Served(ctx, server, opts.port)
    finally:
        ctx.close()
