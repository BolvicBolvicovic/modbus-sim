# What mogbus does

The features at a glance: protocols, data types, behaviours, faults, observability and embedding.

| Feature | Details |
|---|---|
| Protocol | Modbus TCP (optionally TLS-wrapped) and Modbus UDP; Modbus RTU and Modbus ASCII over a PTY/serial device, a TCP byte stream, or UDP datagrams (one frame per datagram). Function codes 01, 02, 03, 04, 05, 06, 07, 08 (public diagnostics subfunctions 0x0000-0x0004, 0x000A-0x0012, 0x0014), 11, 12, 15, 16, 17, 20, 21, 22, 23, 24, 43 (MEI 14 and MEI 13 CANopen SDO upload/download). Exceptions 01, 02, 03, 0B; fault injection adds 04-08 and 0A. User-defined function codes (`(function-code ...)`: fixed reply, echo, exception, live registers or no response). |
| Data | Coils, discrete inputs, input registers, holding registers. 16, 32 and 64-bit integers, float32/float64, ASCII strings, word and byte order (ABCD, CDAB, BADC, DCBA). |
| Behaviours | **static** (fixed value), **ramp** (moves over time: wrap, bounce or hold), **sine**, **clock** (a wall-clock field: year, month, day, weekday, hour, minute, second or unix time), **lag** (first-order lag towards a number or another point, e.g. a heater following its setpoint), an optional **noise** modifier (uniform or gaussian, reproducible), an optional **degradation** modifier (drift, growing noise, stuck value and read failure with exception 04 over time, healed by a restart), and **machine** (bounded scripted state machine, see the [config reference](config/machines/index.md)). |
| Fault | Repeatable per device, one definition per kind: **delay**, **drop**, **exception**, **corrupt** (PDU, register or checksum), **restart**, **sensor-disconnect**, and **disconnect** (TCP-family modes only), each firing by percent chance or a non-repeating `after-ms`/`count` schedule (max 8 steps). |
| Devices | Up to 16 devices on one port or line, told apart by unit id. |
| Clients | Up to 32 TCP connections at a time; UDP modes answer any number of peers on one socket. A delayed response never blocks other clients. |
| Writes | Clients can write coils and holding registers, except points marked `(access read-only)`. Writing a ramp freezes it at the written value. |
| Observability | `(server (http ...))` serves `/healthz`, `/readyz` and Prometheus `/metrics`; `--log-format json` writes one JSON object per log line, with structured request events under `-v`. `(http (control on))` adds an opt-in JSON API to read and write registers and set or clear faults while the server runs. |
| Embedding | `libmogbus.a` / `libmogbus.so` with a small C API (`include/mogbus.h`), and a Python package on top of it (`python/`, for pytest and scripts); both include a Modbus client (master) for every network transport and serial lines, with typed calls, raw requests, timeouts, retries and reconnects; the `mogbus` binary is itself a client of the C API. |

## How it is built

Plain C (gnu11) with the [vlib](https://github.com/BolvicBolvicovic/vlib) toolchain. All memory comes
from vlib arenas; the embedded s7 interpreter manages its own memory internally and is only alive
while the config is read. Linux first (the OS calls live in `src/os`); on Windows run it in
[Docker Desktop or WSL2](windows/windows-route.md), or try the experimental
[native build](windows/windows-native.md) of the network modes.

## Where to read more

New here? The [pumping station tutorial](tutorial-app.md) walks through most of these features.

| Feature | Page |
|---|---|
| Protocol | [Modbus support](modbus/modbus-overview.md), [Function codes](modbus/function-codes.md), [Transports](transports/transports.md) |
| Data, behaviours, faults | [Config reference](config/format.md), [Faults on the wire](modbus/fault-effects.md) |
| Observability | [Health, readiness and metrics](observability/http-endpoint.md), [Metrics](observability/metrics/metrics.md), [Control API](observability/api/control-api.md), [Logs](observability/logs.md) |
| Embedding | [Embedding mogbus in C](library/library-start.md), [Using mogbus from Python](python/python-start.md), [C client](library/library-client.md), [Python client](python/python-client.md) |
