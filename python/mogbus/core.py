"""Pythonic wrapper over the mogbus C API (see include/mogbus.h)."""
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
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, Iterator, List, NamedTuple, Optional, Sequence, Union

from . import _lib
from ._lib import lib

ERR_SIZE = 512


class MogbusError(Exception):
    """A failed mogbus call. `status` is the MogbusStatus of a failed server creation, else None."""

    def __init__(self, message: str, status: Optional["Status"] = None):
        super().__init__(message)
        self.status = status


class Space(enum.IntEnum):
    COIL = 0
    DISCRETE = 1
    INPUT = 2
    HOLDING = 3


class ValueType(enum.IntEnum):
    U16 = 0
    S16 = 1
    U32 = 2
    S32 = 3
    F32 = 4
    FLOAT32 = 4  # alias of F32, as in the config file
    BOOL = 5
    U64 = 6
    S64 = 7
    F64 = 8
    FLOAT64 = 8  # alias of F64, as in the config file
    STRING = 9


class WordOrder(enum.IntEnum):
    HIGH_FIRST = 0
    LOW_FIRST = 1


class ByteOrder(enum.IntEnum):
    BIG = 0
    LITTLE = 1


class RampMode(enum.IntEnum):
    WRAP = 0
    BOUNCE = 1
    HOLD = 2


class Mode(enum.IntEnum):
    TCP = 0
    RTU_PTY = 1
    RTU_DEVICE = 2
    RTU_TCP = 3
    ASCII_PTY = 4
    ASCII_DEVICE = 5
    ASCII_TCP = 6
    RTU_FD = 7
    ASCII_FD = 8
    UDP = 9
    ASCII_UDP = 10
    RTU_UDP = 11


class Parity(enum.IntEnum):
    NONE = 0
    EVEN = 1
    ODD = 2
    MARK = 3
    SPACE = 4


class TlsVersion(enum.IntEnum):
    V1_2 = 0
    V1_3 = 1


class FaultKind(enum.IntEnum):
    DELAY = 0
    DROP = 1
    EXCEPTION = 2
    CORRUPT = 3
    DISCONNECT = 4
    RESTART = 5
    SENSOR_DISCONNECT = 6


class Status(enum.IntEnum):
    OK = 0
    INVALID = 1
    SERIAL = 2
    LISTEN = 3


class LogLevel(enum.IntEnum):
    DEBUG = 0
    INFO = 1
    WARN = 2
    OFF = 3


class LogFormat(enum.IntEnum):
    TEXT = 0
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
class Ramp:
    """A point that moves over time: from_ to `to` by `step` every `every_ms`."""
    from_: float
    to: float
    step: float
    every_ms: int
    mode: Union[str, int, RampMode] = "wrap"

    @classmethod
    def of(cls, value: Union["Ramp", Dict[str, Any]]) -> "Ramp":
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
    scheduled: bool
    percent: int
    delay_ms: int
    exception_code: int


@dataclass
class CustomFunctionCode:
    code: int
    name: str


@dataclass
class DeviceInfo:
    name: str
    unit_id: int
    point_counts: Dict[str, int]
    faults: Dict[str, FaultInfo]
    custom_function_codes: List[CustomFunctionCode]


@dataclass
class ServerOptions:
    """Fields of mogbus_server_options_t with the defaults of mogbus_server_options_defaults()."""
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
                            "tls_require_client_certificate", "http_control"):
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
    """A simulated device. Valid until its Context is closed. Obtain it from a Context."""

    def __init__(self, ctx: "Context", handle: int):
        self._ctx = ctx
        self._h = handle

    def _live(self):
        self._ctx._check_open()
        return self._h

    def __repr__(self):
        return f"<Device unit={self.unit_id}>"

    @property
    @_locked
    def unit_id(self) -> int:
        return self.info().unit_id

    @property
    @_locked
    def name(self) -> str:
        return self.info().name

    @_locked
    def add_point(self, space, address, type="u16", value=0, word_order="high-first", ramp=None,
                  byte_order="big", length=0, text=None):
        """Adds a point. `byte_order` swaps the bytes inside every register; for type "string",
        `length` is the number of registers (1-123) and `text` the initial text."""
        p = _lib.PointC()
        p.space = int(_coerce(Space, space, "space"))
        p.address = _u(address, "address", 65536)
        p.type = int(_coerce(ValueType, type, "type"))
        p.word_order = int(_coerce(WordOrder, word_order, "word_order"))
        p.byte_order = int(_coerce(ByteOrder, byte_order, "byte_order"))
        p.length = _u(length, "length", 1 << 32)
        if text is not None:
            if p.type != ValueType.STRING:
                raise MogbusError("text only applies to type \"string\"")
            p.text = text.encode("latin-1")
        if ramp is None:
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
        space = int(_coerce(Space, space, "space"))
        address = _u(address, "address", 65536)
        out = ctypes.c_double()
        if not lib.mogbus_device_get_value(self._live(), space, address, ctypes.byref(out)):
            raise MogbusError(f"no point starts at {Space(space).name.lower()} {address}")
        return out.value

    @_locked
    def set_value(self, space, address, value: float) -> None:
        space = int(_coerce(Space, space, "space"))
        address = _u(address, "address", 65536)
        if not lib.mogbus_device_set_value(self._live(), space, address, float(value)):
            raise MogbusError(f"no point starts at {Space(space).name.lower()} {address}")

    @_locked
    def get_string(self, space, address) -> str:
        """The text of the string point at `address`, up to its first NUL byte (bytes as Latin-1)."""
        space = int(_coerce(Space, space, "space"))
        address = _u(address, "address", 65536)
        buf = ctypes.create_string_buffer(2 * 123 + 1)
        if not lib.mogbus_device_get_string(self._live(), space, address, buf, len(buf)):
            raise MogbusError(f"no string point starts at {Space(space).name.lower()} {address}")
        return buf.value.decode("latin-1")

    @_locked
    def set_string(self, space, address, text: str) -> None:
        """Replaces the text of a string point, NUL padded; clients can still write it."""
        space = int(_coerce(Space, space, "space"))
        address = _u(address, "address", 65536)
        if not lib.mogbus_device_set_string(self._live(), space, address, text.encode("latin-1")):
            raise MogbusError(f"no string point starts at {Space(space).name.lower()} {address}, "
                              "or the text is longer than its registers")

    @_locked
    def info(self) -> DeviceInfo:
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
            point_counts={Space(i).name.lower(): c.point_count[i] for i in range(4)},
            faults=faults,
            custom_function_codes=fcs)


class Server:
    """A running transport. Create it with Context.serve() or Context.serve_config()."""

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
        """One poll iteration; returns 0 (continue), 1 (done) or 2 (poll error)."""
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
        """Steps until stopped; blocks. Returns 0 after a clean stop, 1 on a poll error.

        By default the context lock is released between steps so other threads can use the
        devices. With exclusive=True the C mogbus_server_run() holds the lock for the whole run.
        """
        if exclusive:
            with self._ctx._lock:
                return lib.mogbus_server_run(self._live())
        return self._loop()

    def start(self) -> "Server":
        """Runs the server on a background thread; stop() or close() joins it."""
        with self._ctx._lock:
            self._live()
            if self._thread is not None:
                raise MogbusError("server thread already started")
            self._stopping = False
            self._thread = threading.Thread(target=self._loop, name="mogbus-server", daemon=True)
            self._thread.start()
        return self

    def stop(self) -> None:
        """Asks the server to stop and joins the background thread, if any. Takes no lock."""
        self._stopping = True
        h = self._h
        if h:
            lib.mogbus_server_stop(h)
        thread = self._thread
        if thread is not None and thread is not threading.current_thread():
            thread.join()
            self._thread = None

    def close(self) -> None:
        """Stops, joins the thread and closes the transport. Safe to call twice."""
        self.stop()
        with self._ctx._lock:
            if self._h:
                lib.mogbus_server_close(self._h)
                self._h = None
                with contextlib.suppress(ValueError):
                    self._ctx._servers.remove(self)

    @property
    def closed(self) -> bool:
        return not self._h

    @property
    @_locked
    def pty_name(self) -> Optional[str]:
        return _decode(lib.mogbus_server_pty_name(self._live()))

    @_locked
    def metrics(self) -> str:
        """Prometheus text of this server's counters."""
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
    """Owns devices and servers. All methods of a context, its devices and its servers share one
    lock, so they may be called from any thread."""

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
        """Closes every server and releases all memory (mogbus_destroy). Safe to call twice."""
        for server in list(self._servers):
            server.close()
        with self._lock:
            if self._h:
                lib.mogbus_destroy(self._h)
                self._h = None
                self._devices.clear()

    @property
    def closed(self) -> bool:
        return not self._h

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        with contextlib.suppress(Exception):
            self.close()

    def load_config(self, path) -> None:
        with self._lock:
            self._check_open()
            err = _err_buf()
            if not lib.mogbus_load_config(self._h, _text(os.fspath(path)), err, ERR_SIZE):
                raise MogbusError(_msg(err, "cannot load config"))

    def add_device(self, unit_id: int, name: Optional[str] = None) -> Device:
        with self._lock:
            self._check_open()
            unit_id = _u(unit_id, "unit_id", 256)
            err = _err_buf()
            handle = lib.mogbus_add_device(self._h, unit_id, _text(name), err, ERR_SIZE)
            if not handle:
                raise MogbusError(_msg(err, "cannot add device"))
            return self._device(handle)

    def find_device(self, unit_id: int) -> Optional[Device]:
        with self._lock:
            self._check_open()
            return self._device(lib.mogbus_find_device(self._h, _u(unit_id, "unit_id", 256)))

    @property
    def devices(self) -> List[Device]:
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
        """The (server ...) section of the loaded config, or None without a config."""
        with self._lock:
            self._check_open()
            c = _lib.ServerOptionsC()
            if not lib.mogbus_config_server_options(self._h, ctypes.byref(c)):
                return None
            return ServerOptions._from_c(c)

    def check_server(self, options: Optional[ServerOptions] = None, **overrides) -> None:
        """Validates options like serve() would, without starting anything; raises MogbusError."""
        options = _merge(options, overrides)
        with self._lock:
            self._check_open()
            c, keep = options._to_c()
            err = _err_buf()
            if not lib.mogbus_server_check(self._h, ctypes.byref(c), err, ERR_SIZE):
                raise MogbusError(_msg(err, "invalid server options"))
            del keep

    def serve(self, options: Optional[ServerOptions] = None, **overrides) -> Server:
        """Creates a server for every device the context has now. Not started: call .start(),
        .run() or .step()."""
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
        """Creates a server from the (server ...) section of the loaded config."""
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
    """True when a server answers one FC 08 request; see mogbus_probe(). Blocks the caller."""
    try:
        probe_or_raise(options, unit_id, timeout_ms)
    except MogbusError:
        return False
    return True


def probe_or_raise(options: ServerOptions, unit_id: int, timeout_ms: int = 2000) -> None:
    c, keep = options._to_c()
    err = _err_buf()
    ok = lib.mogbus_probe(ctypes.byref(c), _u(unit_id, "unit_id", 256),
                          _u(timeout_ms, "timeout_ms", 1 << 32), err, ERR_SIZE)
    del keep
    if not ok:
        raise MogbusError(_msg(err, "probe failed"))


def rtu_timing(baud: int, frame_gap_ms: int = 0) -> RtuTiming:
    t35 = ctypes.c_uint32()
    gap = ctypes.c_uint32()
    lib.mogbus_rtu_timing(_u(baud, "baud", 1 << 32), _u(frame_gap_ms, "frame_gap_ms", 1 << 32),
                          ctypes.byref(t35), ctypes.byref(gap))
    return RtuTiming(t35.value, gap.value)


def library_version() -> str:
    """mogbus_version() of the loaded library."""
    return _lib.LIBRARY_VERSION


_log_ref = None


def set_log_level(level) -> None:
    lib.mogbus_set_log_level(int(_coerce(LogLevel, level, "log level")))


def set_log_format(fmt) -> None:
    lib.mogbus_set_log_format(int(_coerce(LogFormat, fmt, "log format")))


def set_log_sink(callback: Optional[Callable[[LogLevel, str], None]]) -> None:
    """Sends log lines to callback(level, message) instead of stderr; None restores stderr.

    Process-wide. The callback runs on the thread that logs, which is the server thread while a
    server is started, and with the context lock held: do not block in it.
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
    context: Context
    server: Server
    port: int

    def device(self, unit_id: int) -> Device:
        dev = self.context.find_device(unit_id)
        if dev is None:
            raise MogbusError(f"no device with unit id {unit_id}")
        return dev


@contextlib.contextmanager
def serve(config=None, config_text: Optional[str] = None, **options):
    """Context manager for tests: loads a config, serves it from a background thread and cleans up
    on exit. Yields a Served(context, server, port).

    The server uses the config's own (server ...) section; the keyword arguments you pass
    (mode, port, bind_address, any ServerOptions field) override its fields; a config without one
    gets the config defaults (TCP 1502), as in the binary. With no config the server is TCP on a
    free loopback port. In a network mode, port=0 picks a free port.
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
