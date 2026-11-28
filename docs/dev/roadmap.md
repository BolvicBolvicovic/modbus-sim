# Roadmap

Implemented behaviour is documented in [Architecture](architecture.md) and the user docs; only
outstanding work is listed here. When an item is done, document it there and remove it here.

## Protocol validation and compatibility

- Conformance and interoperability runs against commercial PLC/SCADA products, following the method
  of `examples/interop/` (which already checks every FC against pymodbus). Why: supporting an FC does
  not prove interoperability.

## Adoption (gaps compared with pymodbus as a server/simulator)

- pymodbus importer coverage: custom Python actions (code, cannot be imported).
  Why: each one turns into a warning and a manual edit after importing (see
  `docs/user/import/import-pymodbus-servers.md`).
- Windows (see [Windows build](architecture.md#windows-build) for what exists). Why: many PLC and SCADA engineers
  work on Windows. Remaining:
  - COM ports: a COM-port wait of its own (`os_wait` is now a bare `poll`/`WSAPoll` over sockets, so
    this needs a separate wrapper again: overlapped COM I/O plus `WaitForMultipleObjects`, comm events) and `DCB`/`COMMTIMEOUTS` setup in `os_serial_win32.c`; there is no PTY on Windows, so
    tests would use a com0com pair.
  - A portable `test_support`, which would bring most of `TESTS_POSIX_ONLY` to Windows.
  - Tests on `windows-latest` (unit, Python, interop), native COM tests with com0com.
  - OpenSSL (or Schannel) for TLS.
  - Windows release artifacts.
- Gateway/forwarder: a device whose tables are served by a real remote Modbus device. Why: pymodbus
  has `RemoteDeviceContext` for TCP-to-serial gateways.

## Platforms and integration (gaps compared with libmodbus)

- RS-485 line control: `TIOCSRS485` and manual RTS toggling with a delay. Why: half-duplex
  adapters need it, and libmodbus has `modbus_rtu_set_serial_mode` and RTS callbacks.
- macOS and the BSDs. Why: libmodbus builds there natively. Needs a vlib arena backend check and
  `os_serial`/PTY differences.
- Public value helpers (float/int32/int64 from registers in each word and byte order). Why: libmodbus
  has `modbus_get_float_abcd` and friends; mogbus does this only inside points.
- `make install` with a `pkg-config` file. Why: the release ships a tarball only, which makes linking
  against `libmogbus` harder than against libmodbus.
