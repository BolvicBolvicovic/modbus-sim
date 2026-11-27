# Testing your client against the station

Turn the station into a test fixture: a fresh simulated plant per test, simulated time under the test's control, unittest or pytest.

The point of all this is to test *your* code, so the last step is to put the station in a fixture and
write tests for the kind of code you ship. The example has two small functions standing in for it (a
status screen and a setpoint raiser) in
[`test_pump_station.py`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/python/test_pump_station.py).

## A fixture in one function

`running_station()` in the example builds the device, installs the hook, pins the inflow so every run
has the same curve, serves on free loopback ports with the control API on, and closes everything on
exit. The process model is *not* ticking: a test moves simulated time by calling `process.tick()`,
which makes tests fast and deterministic.

```python
@contextlib.contextmanager
def running_station():
    with mogbus.Context() as ctx:
        dev = build_station(ctx)
        process = Process(dev)
        policy = Policy(dev, process)
        dev.set_access_hook(policy)
        dev.set_value("input", INFLOW, 40.0)        # no ramp: the same level curve on every run
        options = mogbus.ServerOptions(port=free_port(), http_bind_address="127.0.0.1",
                                       http_port=free_port(), http_control=True)
        with ctx.serve(options).start() as server:
            yield Station(ctx, dev, process, policy, server, options)
```

Free ports (`port=free_port()`, or `port=0` with \ref mogbus::core::serve "serve()") matter as soon as
tests run in parallel: any two tests may run at once.

## unittest

pytest is not required. Plain `unittest` tests need only the library, and they are what CI runs:

```python
class PumpStationTest(unittest.TestCase):
    def setUp(self):
        manager = ps.running_station()
        self.station = manager.__enter__()
        self.addCleanup(manager.__exit__, None, None, None)
        self.plc = self.station.client()
        self.addCleanup(self.plc.close)

    def test_setpoint_rules(self):
        self.assertTrue(raise_setpoint(self.plc, 1.0))
        self.assertEqual(read_status(self.plc)["setpoint"], 3.5)
        self.assertFalse(raise_setpoint(self.plc, 10.0))          # exception 03
        self.assertEqual(read_status(self.plc)["setpoint"], 3.5)  # unchanged
        self.assertEqual(self.station.policy.refused, 1)

    def test_alarms_event_log_and_fifo(self):
        process = self.station.process
        self.plc.write_register(ps.MODE, ps.MANUAL)
        self.run_until(lambda: process.high)
        self.assertTrue(read_status(self.plc)["high"])
        self.plc.write_coil(ps.RUN_COMMAND, True)
        self.run_until(lambda: process.low)
        self.assertEqual(self.plc.fifo(ps.ALARM_FIFO), [ps.EV_HIGH_ON, ps.EV_LOW_ON])
```

```bash
make python-examples      # builds libmogbus.so, runs pump_station.py and these tests
```

```text
.......
----------------------------------------------------------------------
Ran 7 tests in 0.183s

OK
```

The tests check the station and the client together: that the setpoint raiser copes with
exception 03, that a status screen shows the alarms, that an injected exception 06 reaches the
caller:

```python
def test_client_survives_an_injected_exception(self):
    self.station.http("PUT", "/api/devices/1/faults/exception", {"exception_code": 6})
    with self.assertRaises(ModbusException) as raised:
        read_status(self.plc)
    self.assertEqual(raised.exception.code, 6)
    self.station.http("DELETE", "/api/devices/1/faults/exception")
    self.assertEqual(read_status(self.plc)["site"], "NORTH-01")
```

> **Note:** The library's log is process-wide and goes to stderr. The example silences it with
> `mogbus.set_log_level("off")` in `setUpModule()`; leave it on while debugging, or collect it with a
> [log sink](python-observe.md#step-7-logs-metrics-and-the-control-api).

## pytest

The same fixture as a pytest fixture, in
[`conftest.py`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/python/conftest.py):

```python
@pytest.fixture
def station():
    import pump_station
    mogbus.set_log_level("off")
    with pump_station.running_station() as st:
        yield st
    mogbus.set_log_level("info")
```

and tests that use it, parametrized:

```python
@pytest.mark.parametrize("setpoint", [0.0, 0.4, 4.6, 9.0, float("nan")])
def test_bad_setpoints_are_refused(station, setpoint):
    with station.client() as plc, pytest.raises(ModbusException) as raised:
        plc.write_f32(ps.SETPOINT, setpoint)
    assert raised.value.code == 3
    assert station.dev.get_value("holding", ps.SETPOINT) == 2.5
```

pytest is not a dependency of mogbus, so CI does not run these (`make python-examples` runs the
unittest file); `pip install pytest` and
`PYTHONPATH=python python3 -m pytest examples/python` run everything, the unittest tests too. A
simpler fixture around \ref mogbus::core::serve "serve()" with a config is in
`examples/python/conftest.py` (`meter`).

## Habits that keep tests reliable

- One context per test, closed by a `with` or a cleanup, so ports and threads never leak.
- Move simulated time with a function, not `time.sleep()`: put the model behind `tick(dt)` and keep
  the thread of [step 6](python-threading.md#step-6-the-process-on-a-thread) for demos.
- Use `probe()` or a first request to wait for readiness; the transport listens as soon as
  `serve()` returns, so a client can connect before the first step.
- Assert on the audit trail and on the metrics (`Server.metrics()`) to check what your client sent,
  not only what it received.
- Use faults from the control API (or from a config) to see how your client reacts to exceptions,
  delays, dropped replies and disconnects.

## Watching and rewriting frames

\ref mogbus::core::Server::set_frame_hook "Server.set_frame_hook()" shows every frame a server
receives and sends (the counterpart of pymodbus `trace_packet`) and lets a test rewrite or drop
them, which fixed faults cannot do. The hook gets a `Frame` (`direction`, `mode`, `peer`, `data`,
`capacity`) and returns `None` to pass it on, the replacement bytes, or `b""` to drop it:

```python
def hook(frame):
    log.append((frame.direction.name, frame.data.hex()))
    if frame.direction == mogbus.FrameDirection.SENT:
        return frame.data[:-1] + bytes([frame.data[-1] ^ 0xFF])   # the client sees a wrong last byte

server.set_frame_hook(hook)
```

Received frames are shown before they are validated, so the rewritten bytes are handled as if they
had arrived; sent frames are shown after fault injection and just before they are written. A
replacement longer than `capacity` drops the frame and logs a warning, and an exception in the hook
is printed and lets the frame through. The same rules per transport are in
[Guard clients and keep records](../library/library-hooks.md#watching-and-rewriting-frames).

## Where next

- The \ref core.py "Python API" is the complete reference.
- [Config reference](../config/format.md): machines, faults and schedules for behaviour that code
  should not carry.
- [Profiles](https://github.com/BolvicBolvicovic/mogbus/tree/main/profiles): ready-made devices to
  serve with `mogbus.serve(config=...)`.
- [Runtime control API](../observability/api/control-api.md) and the [web UI](../observability/web-ui.md).
- [Importing from CSV and pymodbus](../import/import-overview.md): generate the device from your
  register list.

## See also

- [Python tutorial](python-start.md)
- [Threads, locks and shutdown](python-threading.md)
- \ref core.py "Python API reference"
