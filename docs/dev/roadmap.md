# Roadmap

Implemented behaviour is documented in [Architecture](architecture.md) and the user docs; only
outstanding work is listed here. When an item is done, document it there and remove it here.

## RTU timing

- Model true UART character-level timing. Why: t3.5/t1.5 are derived from baud and read
  boundaries, which cannot reproduce byte-time-sensitive serial framing. RTU-over-TCP and
  RTU-over-UDP need no change because they are explicit separate transports.

## Protocol validation and compatibility

- Conformance and interoperability runs against commercial PLC/SCADA products, following the method
  of `examples/interop/` (which already checks every FC against pymodbus). Why: supporting an FC does
  not prove interoperability.

## Adoption (gaps compared with pymodbus as a server/simulator)

- Windows (see [Windows build](architecture.md#windows-build) for what exists). Why: many PLC and SCADA engineers
  work on Windows. Remaining:
  - COM ports: the `os_wait` serial entry (overlapped COM I/O plus `WaitForMultipleObjects`, comm
    events) and `DCB`/`COMMTIMEOUTS` setup in `os_serial_win32.c`; there is no PTY on Windows, so
    tests would use a com0com pair.
  - A portable `test_support`, which would bring most of `TESTS_POSIX_ONLY` to Windows.
  - Tests on `windows-latest` (unit, Python, interop), native COM tests with com0com.
  - OpenSSL (or Schannel) for TLS.
  - Windows release artifacts.
- A connection open/close hook per server (the counterpart of the frame hook). Why: pymodbus has
  `trace_connect`, which tells a host when a client connects or leaves.
- asyncio integration for the Python package. Why: `Server.start()` steps on a background thread, so
  it does not plug into an existing event loop as pymodbus does.
- A frame decoder command (hex frame in, decoded MBAP/RTU/ASCII ADU and PDU out). Why: pymodbus ships
  a message decoder, and it helps when reading captures.
- Gateway/forwarder: a device whose tables are served by a real remote Modbus device. Why: pymodbus
  has `RemoteDeviceContext` for TCP-to-serial gateways.

## Client side (gaps compared with libmodbus and pymodbus)

- TLS in the client (`tls_enabled` is refused today). Why: `net_tls` only has the server side; the
  client needs `TLS_client_method`, CA verification and an optional client certificate.
- Typed client calls for the remaining function codes (07, 08, 0B, 0C, 11, 14, 15, 18, 2B/0D). Why:
  only `mogbus_client_request` reaches them today; follow-up of "more data" in device identification.
- Byte (inter-character) timeouts and host names in the client. Why: libmodbus has
  `modbus_set_byte_timeout`, and only IP literals are accepted today.

## Platforms and integration (gaps compared with libmodbus)

- RS-485 line control: `TIOCSRS485` and manual RTS toggling with a delay. Why: half-duplex
  adapters need it, and libmodbus has `modbus_rtu_set_serial_mode` and RTS callbacks.
- macOS and the BSDs. Why: libmodbus builds there natively. Needs a vlib arena backend check and
  `os_serial`/PTY differences.
- Host names in `bind` (getaddrinfo). Why: only IPv4/IPv6 literals are accepted today.
- Public value helpers (float/int32/int64 from registers in each word and byte order). Why: libmodbus
  has `modbus_get_float_abcd` and friends; mogbus does this only inside points.
- `make install` with a `pkg-config` file. Why: the release ships a tarball only, which makes linking
  against `libmogbus` harder than against libmodbus.
