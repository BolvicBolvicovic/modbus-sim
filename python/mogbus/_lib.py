"""Finds and loads libmogbus.so and declares its C signatures."""
import ctypes
import ctypes.util
import os
from pathlib import Path

VERSION = "0.0.1"


class _FaultInfo(ctypes.Structure):
    _fields_ = [
        ("present", ctypes.c_bool),
        ("scheduled", ctypes.c_bool),
        ("percent", ctypes.c_uint32),
        ("delay_ms", ctypes.c_uint32),
        ("exception_code", ctypes.c_uint8),
    ]


class _CustomFc(ctypes.Structure):
    _fields_ = [("code", ctypes.c_uint8), ("name", ctypes.c_char_p)]


class DeviceInfoC(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("unit_id", ctypes.c_uint8),
        ("any_unit", ctypes.c_bool),
        ("point_count", ctypes.c_uint32 * 4),
        ("faults", _FaultInfo * 7),
        ("custom_fc_count", ctypes.c_uint32),
        ("custom_fcs", _CustomFc * 8),
    ]


class _ActionParam(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("is_bool", ctypes.c_bool),
        ("flag", ctypes.c_bool),
        ("number", ctypes.c_double),
    ]


class ActionC(ctypes.Structure):
    _fields_ = [
        ("space", ctypes.c_int),
        ("address", ctypes.c_uint32),
        ("on_write", ctypes.c_bool),
        ("name", ctypes.c_char_p),
        ("param_count", ctypes.c_uint32),
        ("params", _ActionParam * 16),
    ]


class _Ramp(ctypes.Structure):
    _fields_ = [
        ("from_", ctypes.c_double),
        ("to", ctypes.c_double),
        ("step", ctypes.c_double),
        ("every_ms", ctypes.c_uint32),
        ("mode", ctypes.c_int),
    ]


class PointC(ctypes.Structure):
    _fields_ = [
        ("space", ctypes.c_int),
        ("address", ctypes.c_uint32),
        ("type", ctypes.c_int),
        ("word_order", ctypes.c_int),
        ("kind", ctypes.c_int),
        ("value", ctypes.c_double),
        ("ramp", _Ramp),
        ("byte_order", ctypes.c_int),
        ("length", ctypes.c_uint32),
        ("text", ctypes.c_char_p),
        ("read_only", ctypes.c_bool),
        ("clock_field", ctypes.c_int),
        ("clock_utc", ctypes.c_bool),
        ("note", ctypes.c_char_p),
    ]


class ServerOptionsC(ctypes.Structure):
    _fields_ = [
        ("mode", ctypes.c_int),
        ("bind_address", ctypes.c_char_p),
        ("port", ctypes.c_uint16),
        ("path", ctypes.c_char_p),
        ("fd", ctypes.c_int),
        ("seed", ctypes.c_uint32),
        ("idle_timeout_ms", ctypes.c_uint32),
        ("frame_gap_ms", ctypes.c_uint32),
        ("baud", ctypes.c_uint32),
        ("strict_t15", ctypes.c_bool),
        ("broadcast_execute", ctypes.c_bool),
        ("data_bits", ctypes.c_uint32),
        ("parity", ctypes.c_int),
        ("stop_bits", ctypes.c_uint32),
        ("tls_enabled", ctypes.c_bool),
        ("tls_handshake_timeout_ms", ctypes.c_uint32),
        ("tls_certificate", ctypes.c_char_p),
        ("tls_private_key", ctypes.c_char_p),
        ("tls_ca_certificate", ctypes.c_char_p),
        ("tls_require_client_certificate", ctypes.c_bool),
        ("tls_min_version", ctypes.c_int),
        ("tls_ciphers_1_2", ctypes.c_char_p),
        ("tls_ciphersuites_1_3", ctypes.c_char_p),
        ("http_bind_address", ctypes.c_char_p),
        ("http_port", ctypes.c_uint16),
        ("http_control", ctypes.c_bool),
        ("http_ui", ctypes.c_bool),
        ("http_allowed_hosts", ctypes.c_char_p),
        ("id", ctypes.c_char_p),
        ("show_registers_ms", ctypes.c_uint32),
        ("char_timing", ctypes.c_bool),
    ]


class PollFdC(ctypes.Structure):
    _fields_ = [("fd", ctypes.c_uint64), ("events", ctypes.c_uint32)]


class ClientOptionsC(ctypes.Structure):
    _fields_ = [
        ("mode", ctypes.c_int),
        ("host", ctypes.c_char_p),
        ("port", ctypes.c_uint16),
        ("path", ctypes.c_char_p),
        ("baud", ctypes.c_uint32),
        ("data_bits", ctypes.c_uint32),
        ("parity", ctypes.c_int),
        ("stop_bits", ctypes.c_uint32),
        ("tls_enabled", ctypes.c_bool),
        ("timeout_ms", ctypes.c_uint32),
        ("retries", ctypes.c_uint32),
        ("byte_timeout_ms", ctypes.c_uint32),
        ("tls_ca_file", ctypes.c_char_p),
        ("tls_cert_file", ctypes.c_char_p),
        ("tls_key_file", ctypes.c_char_p),
        ("tls_server_name", ctypes.c_char_p),
        ("tls_verify", ctypes.c_bool),
    ]


class ClientFileRecordC(ctypes.Structure):
    _fields_ = [
        ("file", ctypes.c_uint16),
        ("record", ctypes.c_uint16),
        ("length", ctypes.c_uint16),
        ("values", ctypes.POINTER(ctypes.c_uint16)),
    ]


class AccessC(ctypes.Structure):
    _fields_ = [
        ("kind", ctypes.c_int),
        ("space", ctypes.c_int),
        ("address", ctypes.c_uint32),
        ("count", ctypes.c_uint32),
        ("values", ctypes.POINTER(ctypes.c_uint16)),
        ("function_code", ctypes.c_uint8),
        ("target", ctypes.c_int),
        ("file", ctypes.c_uint16),
        ("node", ctypes.c_uint8),
        ("subindex", ctypes.c_uint8),
        ("index", ctypes.c_uint16),
        ("data", ctypes.POINTER(ctypes.c_uint8)),
        ("data_len", ctypes.c_uint32),
    ]


class FrameC(ctypes.Structure):
    _fields_ = [
        ("direction", ctypes.c_int),
        ("mode", ctypes.c_int),
        ("peer", ctypes.c_char_p),
        ("data", ctypes.POINTER(ctypes.c_uint8)),
        ("len", ctypes.c_size_t),
        ("capacity", ctypes.c_size_t),
    ]


class ConnectionC(ctypes.Structure):
    _fields_ = [
        ("connected", ctypes.c_bool),
        ("id", ctypes.c_uint64),
        ("mode", ctypes.c_int),
        ("peer", ctypes.c_char_p),
        ("reason", ctypes.c_int),
    ]


ACCESS_FN = ctypes.CFUNCTYPE(ctypes.c_uint8, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(AccessC))
FRAME_FN = ctypes.CFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(FrameC))
CONNECTION_FN = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ConnectionC))
WRITTEN_FN = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(AccessC))
FUNCTION_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint8,
                              ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint8),
                              ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))
LOG_FN = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p)


def _candidates():
    env = os.environ.get("MOGBUS_LIB")
    if env:
        # An explicit path never falls back silently.
        return [env], True
    here = Path(__file__).resolve().parent
    found = [str(here / "libmogbus.so"), str(here.parent.parent / "libmogbus.so")]
    found.append(ctypes.util.find_library("mogbus"))
    return [c for c in found if c], False


def _load():
    candidates, explicit = _candidates()
    tried = []
    for cand in candidates:
        if os.sep in cand and not os.path.exists(cand):
            tried.append(f"{cand} (missing)")
            continue
        try:
            return ctypes.CDLL(cand)
        except OSError as exc:
            tried.append(f"{cand} ({exc})")
    where = "MOGBUS_LIB" if explicit else "the package directory, the checkout root or the system"
    raise ImportError(
        "cannot load libmogbus.so from " + where + "; tried: " + ", ".join(tried or ["nothing"]) +
        ". Build it with `make lib` and set MOGBUS_LIB=/path/to/libmogbus.so, "
        "or run `make python` and install ./python.")


lib = _load()

_c = ctypes
_p = ctypes.POINTER
_ptr = _c.c_void_p


def _sig(name, restype, *argtypes):
    fn = getattr(lib, name)
    fn.restype = restype
    fn.argtypes = list(argtypes)


_sig("mogbus_version", _c.c_char_p)
_sig("mogbus_create", _c.c_int, _p(_ptr))
_sig("mogbus_destroy", None, _ptr)
_sig("mogbus_last_error", _c.c_char_p, _ptr)
_sig("mogbus_load_config", _c.c_int, _ptr, _c.c_char_p)
_sig("mogbus_add_device", _c.c_int, _ptr, _c.c_uint8, _c.c_char_p, _p(_ptr))
_sig("mogbus_find_device", _ptr, _ptr, _c.c_uint8)
_sig("mogbus_device_count", _c.c_size_t, _ptr)
_sig("mogbus_device_at", _ptr, _ptr, _c.c_size_t)
_sig("mogbus_device_info", _c.c_bool, _ptr, _p(DeviceInfoC))
_sig("mogbus_device_point_at", _c.c_bool, _ptr, _c.c_uint32, _p(PointC))
_sig("mogbus_device_action_at", _c.c_bool, _ptr, _c.c_uint32, _p(ActionC))
_sig("mogbus_device_set_action_param", _c.c_int, _ptr, _c.c_int, _c.c_uint32, _c.c_bool, _c.c_char_p,
     _c.c_double)
_sig("mogbus_device_get_action_param", _c.c_int, _ptr, _c.c_int, _c.c_uint32, _c.c_bool, _c.c_char_p,
     _p(_c.c_double))
_sig("mogbus_device_add_point", _c.c_int, _ptr, _p(PointC))
_sig("mogbus_device_read", _c.c_int, _ptr, _c.c_int, _c.c_uint32, _c.c_uint32, _p(_c.c_uint16))
_sig("mogbus_device_write", _c.c_int, _ptr, _c.c_int, _c.c_uint32, _c.c_uint32, _p(_c.c_uint16))
_sig("mogbus_device_get_value", _c.c_int, _ptr, _c.c_int, _c.c_uint32, _p(_c.c_double))
_sig("mogbus_device_set_value", _c.c_int, _ptr, _c.c_int, _c.c_uint32, _c.c_double)
_sig("mogbus_device_get_string", _c.c_int, _ptr, _c.c_int, _c.c_uint32, _c.c_char_p, _c.c_size_t)
_sig("mogbus_device_set_string", _c.c_int, _ptr, _c.c_int, _c.c_uint32, _c.c_char_p)
_sig("mogbus_device_add_server", _c.c_int, _ptr, _c.c_char_p)
_sig("mogbus_device_set_any_unit", _c.c_int, _ptr, _c.c_bool)
_sig("mogbus_device_set_access_hook", _c.c_int, _ptr, ACCESS_FN, _ptr)
_sig("mogbus_device_set_written_hook", _c.c_int, _ptr, WRITTEN_FN, _ptr)
_sig("mogbus_device_add_function_code", _c.c_int, _ptr, _c.c_uint8, FUNCTION_FN, _ptr)
_sig("mogbus_device_add_file_record", _c.c_int, _ptr, _c.c_uint16, _c.c_uint16, _c.c_uint16,
     _p(_c.c_uint16))
_sig("mogbus_device_read_file_records", _c.c_int, _ptr, _c.c_uint16, _c.c_uint16, _c.c_uint16,
     _p(_c.c_uint16))
_sig("mogbus_device_write_file_records", _c.c_int, _ptr, _c.c_uint16, _c.c_uint16, _c.c_uint16,
     _p(_c.c_uint16))
_sig("mogbus_device_add_fifo", _c.c_int, _ptr, _c.c_uint16, _c.c_uint16, _p(_c.c_uint16))
_sig("mogbus_device_set_fifo", _c.c_int, _ptr, _c.c_uint16, _c.c_uint16, _p(_c.c_uint16))
_sig("mogbus_device_add_canopen_object", _c.c_int, _ptr, _c.c_uint8, _c.c_uint16, _c.c_uint8,
     _c.c_uint16, _p(_c.c_uint8))
_sig("mogbus_device_read_canopen", _c.c_int, _ptr, _c.c_uint8, _c.c_uint16, _c.c_uint8,
     _c.c_uint16, _c.c_uint16, _p(_c.c_uint8))
_sig("mogbus_device_write_canopen", _c.c_int, _ptr, _c.c_uint8, _c.c_uint16, _c.c_uint8,
     _c.c_uint16, _c.c_uint16, _p(_c.c_uint8))
_sig("mogbus_server_options_defaults", ServerOptionsC)
_sig("mogbus_server_create", _c.c_int, _ptr, _p(ServerOptionsC), _p(_ptr))
_sig("mogbus_server_create_from_config", _c.c_int, _ptr, _p(_ptr))
_sig("mogbus_config_server_options", _c.c_int, _ptr, _p(ServerOptionsC))
_sig("mogbus_server_check", _c.c_int, _ptr, _p(ServerOptionsC))
_sig("mogbus_probe", _c.c_int, _p(ServerOptionsC), _c.c_uint8, _c.c_uint32)
_sig("mogbus_client_options_defaults", ClientOptionsC)
_sig("mogbus_client_open", _c.c_int, _p(ClientOptionsC), _p(_ptr))
_sig("mogbus_client_close", None, _ptr)
_sig("mogbus_client_last_error", _c.c_char_p, _ptr)
_sig("mogbus_client_last_exception", _c.c_uint8, _ptr)
_sig("mogbus_client_connect", _c.c_int, _ptr)
_sig("mogbus_client_request", _c.c_int, _ptr, _c.c_uint8, _c.c_uint8, _p(_c.c_uint8), _c.c_size_t,
     _p(_c.c_uint8), _c.c_size_t, _p(_c.c_size_t))
_sig("mogbus_client_read_coils", _c.c_int, _ptr, _c.c_uint8, _c.c_uint16, _c.c_uint16, _p(_c.c_uint8))
_sig("mogbus_client_read_discrete_inputs", _c.c_int, _ptr, _c.c_uint8, _c.c_uint16, _c.c_uint16,
     _p(_c.c_uint8))
_sig("mogbus_client_read_holding_registers", _c.c_int, _ptr, _c.c_uint8, _c.c_uint16, _c.c_uint16,
     _p(_c.c_uint16))
_sig("mogbus_client_read_input_registers", _c.c_int, _ptr, _c.c_uint8, _c.c_uint16, _c.c_uint16,
     _p(_c.c_uint16))
_sig("mogbus_client_write_coil", _c.c_int, _ptr, _c.c_uint8, _c.c_uint16, _c.c_bool)
_sig("mogbus_client_write_register", _c.c_int, _ptr, _c.c_uint8, _c.c_uint16, _c.c_uint16)
_sig("mogbus_client_write_coils", _c.c_int, _ptr, _c.c_uint8, _c.c_uint16, _c.c_uint16, _p(_c.c_uint8))
_sig("mogbus_client_write_registers", _c.c_int, _ptr, _c.c_uint8, _c.c_uint16, _c.c_uint16,
     _p(_c.c_uint16))
_sig("mogbus_client_mask_write_register", _c.c_int, _ptr, _c.c_uint8, _c.c_uint16, _c.c_uint16,
     _c.c_uint16)
_sig("mogbus_client_read_write_registers", _c.c_int, _ptr, _c.c_uint8, _c.c_uint16, _c.c_uint16,
     _p(_c.c_uint16), _c.c_uint16, _c.c_uint16, _p(_c.c_uint16))
_sig("mogbus_client_read_device_id", _c.c_int, _ptr, _c.c_uint8, _p(_c.c_uint8), _c.c_size_t,
     _p(_c.c_size_t))
_sig("mogbus_client_read_device_identification", _c.c_int, _ptr, _c.c_uint8, _c.c_uint8, _c.c_uint8,
     _p(_c.c_uint8), _c.c_size_t, _p(_c.c_size_t), _p(_c.c_uint8))
_sig("mogbus_client_read_exception_status", _c.c_int, _ptr, _c.c_uint8, _p(_c.c_uint8))
_sig("mogbus_client_diagnostics", _c.c_int, _ptr, _c.c_uint8, _c.c_uint16, _p(_c.c_uint8), _c.c_size_t,
     _p(_c.c_uint8), _c.c_size_t, _p(_c.c_size_t))
_sig("mogbus_client_get_comm_event_counter", _c.c_int, _ptr, _c.c_uint8, _p(_c.c_uint16),
     _p(_c.c_uint16))
_sig("mogbus_client_get_comm_event_log", _c.c_int, _ptr, _c.c_uint8, _p(_c.c_uint16), _p(_c.c_uint16),
     _p(_c.c_uint16), _p(_c.c_uint8), _c.c_size_t, _p(_c.c_size_t))
_sig("mogbus_client_report_server_id", _c.c_int, _ptr, _c.c_uint8, _p(_c.c_uint8), _c.c_size_t,
     _p(_c.c_size_t))
_sig("mogbus_client_read_file_records", _c.c_int, _ptr, _c.c_uint8, _p(ClientFileRecordC), _c.c_size_t)
_sig("mogbus_client_write_file_records", _c.c_int, _ptr, _c.c_uint8, _p(ClientFileRecordC), _c.c_size_t)
_sig("mogbus_client_read_fifo", _c.c_int, _ptr, _c.c_uint8, _c.c_uint16, _p(_c.c_uint16), _c.c_size_t,
     _p(_c.c_size_t))
_sig("mogbus_client_canopen_request", _c.c_int, _ptr, _c.c_uint8, _p(_c.c_uint8), _c.c_size_t,
     _p(_c.c_uint8), _c.c_size_t, _p(_c.c_size_t))
_sig("mogbus_decode_frame", _c.c_int, _c.c_char_p, _c.c_size_t, _c.c_int, _c.c_int, _c.c_bool, _c.c_char_p,
     _c.c_size_t, _p(_c.c_size_t))
_sig("mogbus_registers_to_value", _c.c_int, _p(_c.c_uint16), _c.c_int, _c.c_int, _c.c_int, _p(_c.c_double))
_sig("mogbus_value_to_registers", _c.c_int, _c.c_double, _c.c_int, _c.c_int, _c.c_int, _p(_c.c_uint16),
     _p(_c.c_size_t))
_sig("mogbus_registers_to_u64", _c.c_int, _p(_c.c_uint16), _c.c_int, _c.c_int, _p(_c.c_uint64))
_sig("mogbus_u64_to_registers", _c.c_int, _c.c_uint64, _c.c_int, _c.c_int, _p(_c.c_uint16))
_sig("mogbus_registers_to_text", _c.c_int, _p(_c.c_uint16), _c.c_size_t, _c.c_int, _c.c_char_p, _c.c_size_t)
_sig("mogbus_text_to_registers", _c.c_int, _c.c_char_p, _c.c_size_t, _c.c_int, _p(_c.c_uint16))
_sig("mogbus_rtu_timing", None, _c.c_uint32, _c.c_uint32, _p(_c.c_uint32), _p(_c.c_uint32))
_sig("mogbus_now_ms", _c.c_uint64)
_sig("mogbus_server_step", _c.c_int, _ptr, _c.c_int)
_sig("mogbus_step", _c.c_int, _ptr, _c.c_int)
_sig("mogbus_server_stop", None, _ptr)
_sig("mogbus_server_poll_fds", _c.c_int, _ptr, _p(PollFdC), _c.c_size_t, _p(_c.c_size_t), _p(_c.c_int))
_sig("mogbus_poll_fds", _c.c_int, _ptr, _p(PollFdC), _c.c_size_t, _p(_c.c_size_t), _p(_c.c_int))
_sig("mogbus_server_run", _c.c_int, _ptr)
_sig("mogbus_server_close", None, _ptr)
_sig("mogbus_server_metrics", _c.c_size_t, _ptr, _c.c_char_p, _c.c_size_t)
_sig("mogbus_server_pty_name", _c.c_char_p, _ptr)
_sig("mogbus_server_set_frame_hook", _c.c_int, _ptr, FRAME_FN, _ptr)
_sig("mogbus_server_set_connection_hook", _c.c_int, _ptr, CONNECTION_FN, _ptr)
_sig("mogbus_set_log_sink", None, LOG_FN, _ptr)
_sig("mogbus_set_log_level", None, _c.c_int)
_sig("mogbus_set_log_format", None, _c.c_int)

LIBRARY_VERSION = lib.mogbus_version().decode()
if LIBRARY_VERSION.split(".")[:2] != VERSION.split(".")[:2]:
    raise ImportError(
        f"libmogbus {LIBRARY_VERSION} does not match the Python package {VERSION}; "
        "use a library with the same major.minor version")
