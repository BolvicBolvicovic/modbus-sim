# Tutorial: simulate a pumping station

Build a simulated water pumping station, run it as a Modbus server, and use it to test a SCADA system or a data logger without the real PLC. About thirty minutes.

The station has one tank and one pump. The tank fills at a steady rate and the pump drains it. In auto
mode the pump starts when the level is 0.3 m above the setpoint and stops 0.3 m below it. The finished
config is [`examples/tutorial/pump-station.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/tutorial/pump-station.scm),
and it is checked with `mogbus --check` on every change, so it cannot rot. The same station is built from [C](library/library-start.md) and
[Python](python/python-start.md) elsewhere; here it is only a config file and the `mogbus` binary.

You need a built `./mogbus` ([Get started](get-started.md), step 1). Run every command from the
repository root.

## The register map

Unit id 1, "Pump station":

| Table | Address | Type | Meaning |
|---|---|---|---|
| holding | 0-1 | float32 | level setpoint (m), default 2.5, writable |
| holding | 2 | u16 | mode: 0 off, 1 auto, 2 manual (default 1) |
| holding | 10-17 | string, 8 registers | site name, `NORTH-01`, read-only |
| coil | 0 | bit | pump run command (manual mode) |
| discrete | 0 | bit | high-level alarm |
| discrete | 1 | bit | low-level alarm |
| discrete | 2 | bit | pump running |
| input | 0-1 | float32 | tank level (m) |
| input | 2-3 | float32 | outflow (m3/h) |
| input | 4 | u16 | motor current, 0.1 A |
| input | 10-11 | u32 | pump run seconds |
| input | 20-21 | u32 | wall clock, unix seconds |

## 1. The points

A config is a list of `(name value)` entries (see the [config format](config/format.md)). Start with
the points that never move. Save this as `station.scm`:

```scheme
(server (mode tcp) (port 1502))

(device
  (name "Pump station")
  (unit-id 1)

  (holding 0 (type float32) (static 2.5))      ; two registers, high word first
  (holding 2 (type u16) (static 1))
  (holding 10 (type string) (length 8) (access read-only) (static "NORTH-01"))
  (coil 0)
  (discrete 0)
  (discrete 1)
  (discrete 2)
  (input 0 (type float32) (static 2.5)))
```

- A [`(type float32)`](config/points/type.md) point spans two registers; the
  [word order](config/points/word-order.md) is high word first unless you say otherwise.
- A [string](config/points/string.md) is N registers of two ASCII characters.
- [`(access read-only)`](config/points/access.md) makes every client write to the point fail. Only
  `coil` and `holding` points take it; input registers and discrete inputs are always read-only to
  clients.
- A point with no behaviour is a static point with value 0 (or off).

Only the points you configure exist. Every other address is undefined, and a client that touches it
gets exception 02.

## 2. Check the config

```bash
./mogbus --check=verbose station.scm
```

```text
1 device on 127.0.0.1:1502
  unit 1   Pump station                     coils 1, discrete 3, input 1, holding 3, fault: none
config OK
```

`--check=verbose` prints the same summary as a real start (plain `--check` prints nothing when the file is valid) and validates everything it can without opening a
socket. A mistake is reported with its position in the file; see [config errors](config/errors.md).
All options are on the [command line](command-line.md) page.

## 3. Make it move

Replace the single input with the live values. Outflow is a noisy sine, the motor current follows the
"pump running" bit, and the clock register shows the time of the machine running mogbus:

```scheme
(server (mode tcp) (port 1502) (seed 7))

(device
  (name "Pump station")
  (unit-id 1)

  (holding 0 (type float32) (static 2.5))
  (holding 2 (type u16) (static 1))
  (holding 10 (type string) (length 8) (access read-only) (static "NORTH-01"))
  (coil 0)
  (discrete 0)
  (discrete 1)
  (discrete 2)

  (input 0 (type float32) (static 2.5))
  (input 2 (type float32)
    (sine (offset 40) (amplitude 6) (period-ms 120000))
    (noise (amplitude 0.8) (every-ms 500) (distribution gaussian)))
  (input 4 (type u16)
    (lag (target (discrete 2)) (tau-ms 1500) (gain 120))
    (noise (amplitude 2) (every-ms 250)))
  (input 10 (type u32) (static 0))
  (input 20 (type u32) (clock unix)))
```

- [`sine`](config/behaviours/sine.md) is `offset + amplitude * sin(...)`: 40 m3/h plus or minus 6 over
  a two-minute period. [`noise`](config/behaviours/noise.md) adds a reproducible disturbance on top.
- [`lag`](config/behaviours/lag.md) follows a goal like a first-order system: the current heads for
  `120` (12.0 A) while the "pump running" bit is 1 and back to 0 when it is 0, closing 63% of the gap
  every 1.5 s. A target can be any configured point.
- [`clock`](config/behaviours/clock.md) shows `unix` seconds; it needs an integer type of at least 32
  bits.
- `(seed 7)` makes the random parts of machines reproducible (see [seed](config/server/seed.md)); the
  noise is reproducible on its own.

Behaviours are computed when a request arrives, not on a timer, so an idle server costs nothing.

## 4. Let the pump drive the level

Level and run seconds should depend on what the pump does. That is a job for a
[machine](config/machines/index.md): a small state machine that owns some points. Add it inside the
device, next to the points:

```scheme
  (machine pump
    (initial stopped)
    (var seconds 0)

    (state stopped
      (on-enter (set! (discrete 2) #f))
      (on-tick (set! (input 0) (min 5 (+ (input 0) (/ (* 0.02 (elapsed-ms)) 1000))))))
    (state running
      (on-enter (set! (discrete 2) #t))
      (on-tick (set! (input 0) (max 0 (- (input 0) (/ (* 0.04 (elapsed-ms)) 1000))))
               (set! seconds (+ seconds (/ (elapsed-ms) 1000)))
               (set! (input 10) seconds)))

    (transition stopped running
      (when (or (and (= (holding 2) 1) (> (input 0) (+ (holding 0) 0.3)))
                (and (= (holding 2) 2) (coil 0)))))
    (transition running stopped
      (when (or (= (holding 2) 0)
                (and (= (holding 2) 1) (< (input 0) (- (holding 0) 0.3)))
                (and (= (holding 2) 2) (not (coil 0)))))))

  (machine alarms
    (initial watching)
    (state watching
      (on-tick (set! (discrete 0) (> (input 0) 4))
               (set! (discrete 1) (< (input 0) 1)))))
```

The tank gains 2 cm per second with the pump off and loses 4 cm per second with it on. `elapsed-ms` is
the time since the machine last ran, so the result does not depend on how often a client polls. The
`transition` lines are the control logic: `(holding 2)` is the mode, `(holding 0)` the setpoint and
`(coil 0)` the run command, all of which a client can write. See [state](config/machines/state.md),
[transition](config/machines/transition.md), [statements](config/machines/statements.md) and
[expressions](config/machines/expressions.md).

A point a machine writes belongs to it: a client write to the level or the alarms fails with exception
02, so the simulation keeps control ([point ownership](config/machines/points.md)). The second machine,
`alarms`, shows that machines on one device can read each other's points.

## 5. Add a fault and an HTTP endpoint

A real PLC is sometimes slow. Faults are listed per device, one definition per kind, and fire by
percent or on a schedule (see [faults](config/faults/index.md)):

```scheme
  (fault (kind delay) (delay-ms 800) (percent 2))
```

Two percent of the answers now arrive 800 ms late. A delay does not block the other clients.
To observe the station, add an HTTP endpoint to the `(server ...)` form (see
[`http`](config/server/http.md)):

```scheme
(server (mode tcp) (port 1502) (seed 7)
  (http (port 9102)))
```

The full file has all of this. Check it:

```bash
./mogbus --check=verbose examples/tutorial/pump-station.scm
```

```text
1 device on 127.0.0.1:1502
  HTTP /healthz, /readyz and /metrics on 127.0.0.1:9102
  unit 1   Pump station                     coils 1, discrete 3, input 5, holding 3, fault: delay 2% of responses by 800 ms
config OK
```

## 6. Run it and read it

```bash
./mogbus examples/tutorial/pump-station.scm
```

The server listens on `127.0.0.1:1502` (loopback only; `(bind "0.0.0.0")` opens it to the network).
Leave it running and use another terminal. [`tools/modbus_probe.py`](https://github.com/BolvicBolvicovic/mogbus/blob/main/tools/modbus_probe.py)
is a Modbus TCP client with no dependencies; any other client works the same against
`127.0.0.1:1502`, unit 1.

```bash
./tools/modbus_probe.py holding 0 2 --as float32     # setpoint
./tools/modbus_probe.py holding 10 8 --as string     # site name
./tools/modbus_probe.py discrete 0 3                 # high, low, running
./tools/modbus_probe.py input 0 2 --as float32       # level
./tools/modbus_probe.py input 2 2 --as float32       # outflow
./tools/modbus_probe.py input 20 2 --as u32          # wall clock
```

```text
[2.5]
['NORTH-01']
[0, 0, 0]
[2.5268800258636475]
[40.29308319091797]
[1795777594]
```

Watch the level in auto mode (`--watch` goes before the command). The pump starts at 2.8 m and stops
at 2.2 m:

```bash
./tools/modbus_probe.py --watch 5 input 0 2 --as float32
```

```text
[2.5212199687957764]
[2.6212198734283447]
[2.721219778060913]
[2.821239709854126]
[2.62123966217041]
[2.4211995601654053]
[2.2211995124816895]
[2.0211594104766846]
```

Other transports use the same device with a different `(server (mode ...))`: [RTU over
TCP](transports/transport-rtu-tcp.md), [UDP](transports/transport-udp.md), a PTY for a serial client
(see [RTU](transports/transport-rtu.md)) and [TLS](transports/transport-tls.md). See
[Transports](transports/transports.md).

## 7. Write from a client

Switch to manual mode and start the pump by hand:

```bash
./tools/modbus_probe.py write-reg 2 2        # mode 2 = manual
./tools/modbus_probe.py write-coil 0 on      # run command
./tools/modbus_probe.py discrete 2 1         # running -> [1]
./tools/modbus_probe.py input 10 2 --as u32  # run seconds, counting
```

Poll `input 4` once a second and the motor current climbs towards 12 A (`lag` follows its target as
requests arrive, so a client that polls rarely sees it jump):

```bash
./tools/modbus_probe.py --watch 1 input 4 1
```

```text
[0]
[60]
[90]
[105]
[114]
[117]
```

Writes that a real device would refuse are refused here too:

```bash
./tools/modbus_probe.py write-reg 10 5       # read-only site name
./tools/modbus_probe.py write-reg 100 1      # no such point
```

```text
error: exception 0x02: illegal data address
error: exception 0x02: illegal data address
```

The rules are in [client writes](config/behaviours/client-writes.md) and [Modbus
exceptions](modbus/exceptions.md). Writing a ramp, sine or clock point freezes it at the written
value; this is how a test changes a "sensor".

## 8. The web UI

```bash
./mogbus --web examples/tutorial/pump-station.scm
```

Open <http://127.0.0.1:9102/>. The page lists the points by table with decoded values refreshed every
second (the refresh rate is a selector, down to 10 ms), lets you write decoded or raw values, set
faults, and plot up to eight points as a strip chart: tick the level and the motor current and watch
the pump cycle. `--web` turns on the control API, which has no authentication and logs a WARN; keep it
on loopback. See [Web UI](observability/web-ui.md).

## 9. Health and metrics

The HTTP endpoint answers without a Modbus client:

```bash
curl -s localhost:9102/healthz       # ok
curl -s localhost:9102/readyz        # ready
curl -s localhost:9102/metrics | grep -E '^mogbus_(requests|responses)_total'
```

```text
mogbus_requests_total{fc="2"} 1
mogbus_requests_total{fc="4"} 15
mogbus_responses_total{result="ok"} 16
mogbus_responses_total{result="exception"} 0
```

The full list is in [Metrics](observability/metrics/metrics.md) and the endpoint in [Health, readiness
and metrics](observability/http-endpoint.md). Prometheus scrapes `/metrics`; an orchestrator uses
`/healthz`.

For a probe that goes through the Modbus port itself, use `--health`, which sends one real request to
the first device:

```bash
./mogbus --health examples/tutorial/pump-station.scm; echo $?       # 0
```

The `delay` fault is shorter than the 2 s default timeout, so the probe stays healthy. See [Health
check](deployment/health-check.md).

## 10. Change the station while it runs

With `--web` (or `(http (control on))`) the [control API](observability/api/control-api.md) changes
the running server from a script, which is how a test sets up a scenario. List the devices and the
input points:

```bash
curl -s localhost:9102/api/devices
curl -s 'localhost:9102/api/devices/1/points?space=input'
```

```text
[{"unit":1,"name":"Pump station","points":{"coil":1,"discrete":3,"input":5,"holding":3}}]
{"points":[{"address":0,"registers":2,"type":"float32","kind":"machine","read_only":false,"word_order":"high-first","byte_order":"big"}, ...
```

Play the physical side and force the tank nearly full. A [value write](observability/api/api-set-value.md)
sets the decoded number of a point, like a sensor on the host side:

```bash
curl -s -X PUT -d '{"value":4.5}' localhost:9102/api/devices/1/input/0/value
./tools/modbus_probe.py discrete 0 2
```

```text
{"ok":true}
[1, 0]
```

The high-level alarm is on, and the level stays at 4.5 until the device restarts. Faults are set and
cleared the same way ([set](observability/api/api-set-fault.md),
[clear](observability/api/api-clear-fault.md)):

```bash
curl -s -X PUT -d '{"exception_code":6,"percent":100}' localhost:9102/api/devices/1/faults/exception
./tools/modbus_probe.py holding 2 1
curl -s -X DELETE localhost:9102/api/devices/1/faults/exception
```

```text
{"ok":true}
error: exception 0x06: server device busy
{"ok":true}
```

The same works for `delay`, `drop`, `corrupt` and the other kinds. A `restart` fault resets the device,
which also releases the forced level; clear it afterwards or every request resets again:

```bash
curl -s -X PUT -d '{"percent":100}' localhost:9102/api/devices/1/faults/restart
./tools/modbus_probe.py input 0 2 --as float32      # back at the initial 2.5
curl -s -X DELETE localhost:9102/api/devices/1/faults/restart
```

## 11. Scripted faults

A percent fault is random. For a test that must fail the same way on every run, use a
[schedule](config/faults/schedule.md) (not repeating, at most 8 steps). Add this inside the device to
make the sensors fail five seconds after start, for the next 100 requests:

```scheme
  (fault (kind sensor-disconnect)
    (schedule (step (after-ms 5000) (count 100))))
```

[`sensor-disconnect`](config/faults/sensor-disconnect.md) turns successful reads of the input and
discrete tables into exception 04 while it fires; afterwards reads work again.

## 12. Logs

`-v` logs every request, and `--log-format json` writes one JSON object per line for a log pipeline:

```bash
./mogbus -v --log-format json examples/tutorial/pump-station.scm
```

```text
{"ts":"2026-11-27T11:07:58.505Z","level":"debug","msg":"tcp 127.0.0.1:41718 unit=1 fc=0x04 -> ok","event":"request","transport":"tcp","peer":"127.0.0.1:41718","unit":1,"fc":4,"result":"ok","exception":0,"latency_ms":0,"faults":[]}
```

See [Logs](observability/logs.md).

## 13. Run it in a container

A container needs the server to listen on every interface, so make a copy of the config for it:

```bash
sed 's/(port 1502) (seed 7)/(port 1502) (bind "0.0.0.0") (seed 7)/; s/(http (port 9102))/(http (port 9102) (bind "0.0.0.0"))/' \
  examples/tutorial/pump-station.scm > pump-container.scm
docker build --build-arg VERSION=0.0.1 -t mogbus:0.0.1 .
docker run --rm -p 127.0.0.1:1502:1502 -p 127.0.0.1:9102:9102 \
  -v "$PWD/pump-container.scm:/etc/mogbus/mogbus.scm:ro" mogbus:0.0.1
./tools/modbus_probe.py input 0 2 --as float32      # from the host
```

The mounted file must be readable by UID 10001. The image has a `HEALTHCHECK` that runs `--health`,
runs as an unprivileged user and needs no writable filesystem. See [Docker](deployment/docker.md),
[Docker Compose](deployment/docker-compose.md) and [Kubernetes](deployment/kubernetes.md).

## Where next

- Start from a [profile](profiles/using-profiles.md) instead of a blank file, or
  [import a register map](import/import-overview.md) from a CSV.
- Run several devices behind one port: [unit ids](modbus/unit-ids.md) and
  [`any-unit`](config/device/any-unit.md).
- Add vendor function codes: [user-defined function codes](modbus/user-defined-function-codes.md).
- Embed the same station in a program, [from C](library/library-start.md) or
  [from Python](python/python-start.md), and test it in pytest with the
  [Python testing guide](python/python-testing.md).
- The full [config reference](config/format.md).
