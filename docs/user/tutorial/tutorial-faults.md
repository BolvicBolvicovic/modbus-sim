# Step 5: Break it on purpose

> **Summary:** Make the station slow, silent or wrong the way a real PLC sometimes is: faults in the config, scripted failures that repeat on every run, and the runtime control API that sets them from a test while the server runs.

A client that only ever sees a perfect device is not tested. mogbus injects faults: late answers,
dropped answers, exceptions, corrupted frames, disconnects, restarts and a failing sensor. Faults never
change how a request is processed; they act on the response (a `restart` resets the device after the
response is built).

Faults and machines are the part of the model that the C and Python APIs cannot define: they come from
a config file (load it with @ref mogbus_load_config "mogbus_load_config()" or
\ref mogbus::core::Context::load_config "Context.load_config()", [step 1](tutorial-device.md#from-a-config-file-in-code)),
or are set at run time through the HTTP control API below, which works the same for all three routes.

## A fault in the config

Faults are listed per device, one definition per kind, and fire by percent or on a schedule (see
[faults](../config/faults/index.md)). Add this inside the device:

```scheme
  (fault (kind delay) (delay-ms 800) (percent 2))
```

Two percent of the answers now arrive 800 ms late. A delay does not block the other clients. The same
file also needs the HTTP endpoint on the server to observe the station (see
[`http`](../config/server/http.md)):

```scheme
(server (mode tcp) (port 1502) (seed 7)
  (http (port 9102)))
```

`examples/tutorial/pump-station.scm` has all of this. Check it:

```bash
./mogbus --check=verbose examples/tutorial/pump-station.scm
```

```text
1 device on 127.0.0.1:1502
  HTTP /healthz, /readyz and /metrics on 127.0.0.1:9102
  unit 1   Pump station                     coils 1, discrete 3, input 5, holding 3, fault: delay 2% of responses by 800 ms
config OK
```

## Scripted faults

A percent fault is random. For a test that must fail the same way on every run, use a
[schedule](../config/faults/schedule.md) (not repeating, at most 8 steps). Add this inside the device to
make the sensors fail five seconds after start, for the next 100 requests:

```scheme
  (fault (kind sensor-disconnect)
    (schedule (step (after-ms 5000) (count 100))))
```

[`sensor-disconnect`](../config/faults/sensor-disconnect.md) turns successful reads of the input and
discrete tables into exception 04 while it fires; afterwards reads work again.

## Set faults and values while it runs

The [control API](../observability/api/control-api.md) changes the running server from a script, which
is how a test sets up a scenario. It is off by default (no authentication, a WARN at start): turn it on
with `--web`, `(http (control on))`, or the `http_control` option of the library, and keep it on
loopback. The C program turns on the web UI with `--http PORT` (`http_ui` implies the control API); the
Python program sets `http_control=True`, with the options of step 2:

```bash
./mogbus --web examples/tutorial/pump-station.scm
```

```c
options.http_bind_address = "127.0.0.1";
options.http_port = http_port;
options.http_ui = true;
```

```python
options = mogbus.ServerOptions(port=free_port(), http_bind_address="127.0.0.1", http_port=free_port(),
                               http_control=True)
```

List the devices and the input points:

```bash
curl -s localhost:9102/api/devices
curl -s 'localhost:9102/api/devices/1/points?space=input'
```

```text
[{"unit":1,"name":"Pump station","points":{"coil":1,"discrete":3,"input":5,"holding":3}}]
{"points":[{"address":0,"registers":2,"type":"float32","kind":"machine","read_only":false,"word_order":"high-first","byte_order":"big"}, ...
```

Play the physical side and force the tank nearly full. A [value write](../observability/api/api-set-value.md)
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
cleared the same way ([set](../observability/api/api-set-fault.md),
[clear](../observability/api/api-clear-fault.md)). Here the test makes the station answer exception 06
(busy) to find out what its client does, then clears the fault:

```bash
curl -s -X PUT -d '{"exception_code":6,"percent":100}' localhost:9102/api/devices/1/faults/exception
./tools/modbus_probe.py holding 2 1
curl -s -X DELETE localhost:9102/api/devices/1/faults/exception
```

```python
base = f"http://127.0.0.1:{options.http_port}"
urllib.request.urlopen(urllib.request.Request(
    base + "/api/devices/1/faults/exception", data=b'{"exception_code": 6}', method="PUT"), timeout=2)
try:
    plc.holding(SETPOINT, 2)
except ModbusException as exc:
    print("with the fault injected:", exc)
urllib.request.urlopen(urllib.request.Request(
    base + "/api/devices/1/faults/exception", method="DELETE"), timeout=2)
```

```text
{"ok":true}
error: exception 0x06: server device busy
{"ok":true}
```

The first block is the `curl` route (config and C); the Python program prints `with the fault
injected: function 0x03 failed with exception 06`. The same works for `delay`, `drop`, `corrupt` and the
other kinds. A `restart` fault resets the device, which also releases the forced level; clear it
afterwards or every request resets again:

```bash
curl -s -X PUT -d '{"percent":100}' localhost:9102/api/devices/1/faults/restart
./tools/modbus_probe.py input 0 2 --as float32      # back at the initial 2.5
curl -s -X DELETE localhost:9102/api/devices/1/faults/restart
```

The Python calls change a device from inside the process; the HTTP API is for other processes and for
faults, which the Python API cannot define. See also [using the control API from
Python](../python/python-lifetimes.md#runtime-control).

## Next

- [Step 6: Observe it](tutorial-observe.md)
