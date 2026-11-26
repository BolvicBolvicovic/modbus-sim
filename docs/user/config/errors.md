# Error index

Every message `mogbus --check FILE` can print for a config problem, with the entry that explains it. `--check` reports the first problem and exits.

| Message | Entry |
|---|---|
| `unknown top-level form` | [Overall shape](format.md#overall-shape) |
| `(degradation ...) needs a (static ...), (ramp ...), (sine ...) or (lag ...) to age` | [`(degradation ...)`](behaviours/degradation.md) |
| `degradation needs at least one of drift-per-hour, noise-growth-per-hour, stuck-after-ms and fail-after-ms` | [`(degradation ...)`](behaviours/degradation.md) |
| `noise-growth-per-hour must be 0 or more` | [`(degradation ...)`](behaviours/degradation.md) |
| `coils and discrete inputs only support stuck-after-ms and fail-after-ms` | [`(degradation ...)`](behaviours/degradation.md) |
| `degradation numbers must be finite` | [`(degradation ...)`](behaviours/degradation.md) |
| `fail-after-ms, stuck-after-ms or after-ms out of range` | [`(degradation ...)`](behaviours/degradation.md) |
| `lag target holding 9 is not the first address of a configured point` | [`(lag ...)`](behaviours/lag.md) |
| `a lag cannot follow itself` | [`(lag ...)`](behaviours/lag.md) |
| `(noise ...) needs a (static ...), (ramp ...), (sine ...) or (lag ...) to disturb` | [`(noise ...)`](behaviours/noise.md) |
| `missing required option (to ...)` | [`(ramp ...)`](behaviours/ramp.md) |
| `ramp step must be greater than 0` | [`(ramp ...)`](behaviours/ramp.md) |
| `ramp range is too large for the step` | [`(ramp ...)`](behaviours/ramp.md) |
| `a point can have only one of (static ...), (ramp ...), (sine ...) and (lag ...)` | [`(static VALUE)`](behaviours/static.md) |
| `expected (reply "HEX"), (echo), (exception N), (registers SPACE ADDRESS COUNT) or (no-response)` | [`(function-code (on ...))` actions](device/function-code-actions.md) |
| `the space must be coil, discrete, input or holding` | [`(function-code (on ...))` actions](device/function-code-actions.md) |
| `expected ADDRESS 0-65535 and COUNT 1-125 that stay below 65536` | [`(function-code (on ...))` actions](device/function-code-actions.md) |
| `function code N (0xNN) is implemented by mogbus and cannot be redefined` | [`(device (function-code ...))`](device/function-code.md) |
| `expected (function-code CODE ...) with CODE 1-127` | [`(device (function-code ...))`](device/function-code.md) |
| `needs at least one (on "HEX" ACTION) or ACTION` | [`(device (function-code ...))`](device/function-code.md) |
| `function code is already defined on this device` | [`(device (function-code ...))`](device/function-code.md) |
| `too many user-defined function codes on one device (max 8)` | [`(device (function-code ...))`](device/function-code.md) |
| `too many cases (max 16)` | [`(device (function-code ...))`](device/function-code.md) |
| `"..." is not a list of hex bytes (two digits ...)` | [`(device (function-code ...))`](device/function-code.md) |
| `too many bytes (max 252)` / `(max 16)` | [`(device (function-code ...))`](device/function-code.md) |
| `devices 'A' and 'B' both use unit-id N` | [`(device (unit-id ...))`](device/unit-id.md) |
| `device 'A': missing (unit-id N); every device must set its Modbus unit id` | [`(device (unit-id ...))`](device/unit-id.md) |
| `device 'A': unit-id is given more than once` | [`(device (unit-id ...))`](device/unit-id.md) |
| `corrupt mode checksum is not valid in tcp mode (MBAP frames have no checksum)` | [`(fault (kind corrupt))`](faults/corrupt.md) |
| `mode is only valid for kind corrupt` | [`(fault (kind corrupt))`](faults/corrupt.md) |
| `delay-ms is only valid for kind delay` | [`(fault (kind delay))`](faults/delay.md) |
| `(fault (kind disconnect)) is not valid in ... mode` | [`(fault (kind disconnect))`](faults/disconnect.md) |
| `missing required option (code ...)` | [`(fault (kind exception))`](faults/exception.md) |
| `code is only valid for kind exception` | [`(fault (kind exception))`](faults/exception.md) |
| `code 9 is reserved` | [`(fault (kind exception))`](faults/exception.md) |
| `only one is allowed per kind` | [`(device (fault ...))`](faults/fault.md) |
| `code is only valid for kind exception` | [`(device (fault ...))`](faults/fault.md) |
| `delay-ms is only valid for kind delay` | [`(device (fault ...))`](faults/fault.md) |
| `mode is only valid for kind corrupt` | [`(device (fault ...))`](faults/fault.md) |
| `percent cannot be combined with schedule` | [`(fault (schedule ...))`](faults/schedule.md) |
| `schedule has more than 8 steps` | [`(fault (schedule ...))`](faults/schedule.md) |
| `schedule needs at least one (step ...)` | [`(fault (schedule ...))`](faults/schedule.md) |
| `expected (step (after-ms N) (count N))` | [`(fault (schedule ...))`](faults/schedule.md) |
| `schedule step: missing required option (after-ms ...)` | [`(fault (schedule ...))`](faults/schedule.md) |
| `(after-ms ...): must be between 0 and 86400000` | [`(fault (schedule ...))`](faults/schedule.md) |
| `(count ...): must be between 1 and 1000000` | [`(fault (schedule ...))`](faults/schedule.md) |
| `on-write must follow states and precede transitions` | [`(machine (on-write ...))`](machines/on-write.md) |
| `on-write is given more than once for the same point` | [`(machine (on-write ...))`](machines/on-write.md) |
| `on-write cannot watch a machine-driven point` | [`(machine (on-write ...))`](machines/on-write.md) |
| `only one is allowed per kind` | [`(state (fault ...))`](machines/state-fault.md) |
| `(fault (kind disconnect)) is not valid in ... mode` | [`(state (fault ...))`](machines/state-fault.md) |
| `state-register must immediately follow` | [`(machine (state-register ...))`](machines/state-register.md) |
| `state register cannot name a point driven by a machine` | [`(machine (state-register ...))`](machines/state-register.md) |
| `(access ...)`: `expected one of: read-write, read-only` | [`(holding (access ...))`](points/access.md) |
| `input registers are always read-only for clients: remove (access ...)` / `discrete inputs are always ...` | [`(holding (access ...))`](points/access.md) |
| `(byte-order ...)`: `expected one of: big, little` | [`(holding (byte-order ...))`](points/byte-order.md) |
| `coils and discrete inputs are single bits: remove (byte-order ...)` | [`(holding (byte-order ...))`](points/byte-order.md) |
| `address must be between 0 and 65535` | [`(coil\|discrete\|input\|holding ADDRESS ...)`](points/point.md) |
| `address is already used by another point` | [`(coil\|discrete\|input\|holding ADDRESS ...)`](points/point.md) |
| `unknown option 'statik'. Allowed: ...` | [`(coil\|discrete\|input\|holding ADDRESS ...)`](points/point.md) |
| `expected a number` / `expected a whole number` | [`(coil\|discrete\|input\|holding ADDRESS ...)`](points/point.md) |
| `coils and discrete inputs can only be (static on/off)` | [`(coil\|discrete\|input\|holding ADDRESS ...)`](points/point.md) |
| `syntax error: missing close paren` | [`(coil\|discrete\|input\|holding ADDRESS ...)`](points/point.md) |
| `string points can only be (static "TEXT")` | [`(holding (type string))`](points/string.md) |
| `string points cannot have (noise ...) or (degradation ...)` | [`(holding (type string))`](points/string.md) |
| `missing required option (length ...)` | [`(holding (type string))`](points/string.md) |
| `(length ...): must be between 1 and 123` | [`(holding (type string))`](points/string.md) |
| `text is longer than N bytes (2 per register, length M)` | [`(holding (type string))`](points/string.md) |
| `string text can only use printable ASCII (0x20-0x7E)` | [`(holding (type string))`](points/string.md) |
| `(length ...) and (padding ...) only apply to (type string)` | [`(holding (type string))`](points/string.md) |
| `machine cannot drive a string point` / `a string point has no number to read` / `state register cannot name a string point` / `on-write cannot watch a string point` | [`(holding (type string))`](points/string.md) |
| `strings have no word order: remove (word-order ...)` | [`(holding (word-order ...))`](points/word-order.md) |
| `server: broadcast-execute is not valid in ascii-pty mode (ASCII never executes unit-0 broadcasts)` | [`(server (broadcast-execute ...))`](server/broadcast-execute.md) |
| `server: (path ...) is required in rtu-device and ascii-device modes` | [`(server (path ...))`](server/path.md) |
| `the file has more than one (server ...); only one is allowed` | [`(server ...)`](server/server.md) |
| `server: parity is not valid in tcp mode` | [`(server ...)`](server/server.md) |
| `built without TLS support` | [`(server (tls ...))`](server/tls.md) |
