# Step 7: Test your client

> **Summary:** Turn the station into a test fixture: a fresh simulated plant per test, simulated time under the test's control, and your own client code run against it in CI.

The point of all this is to test *your* code, so the last working step is to put the station in a
fixture and write tests for the kind of code you ship. The routes differ in who starts the station:

- **Config**: a server process, started once for the whole run (or per test script), with a probe to wait for readiness. Behaviour that depends on time (the machines of step 3) is real time.
- **C**: the test program is the server and the client; the tutorial program's `--self-test` steps the server between reads, so there is no second thread or process, and simulated time is a function call.
- **Python**: a fixture in one function builds a context per test, serves it on free loopback ports and closes it on exit, and the model is not ticking, so a test moves simulated time with `process.tick()`.

```bash
./mogbus examples/tutorial/pump-station.scm &
server=$!
until ./mogbus --health examples/tutorial/pump-station.scm; do sleep 0.2; done
./tools/modbus_probe.py discrete 0 3            # or your client under test
kill $server
```

```c
for (int i = 0; i < 2000; i++)
{
	if (mogbus_server_step(server, 1) != MOGBUS_STEP_CONTINUE) return -1;
	if (poll(&pfd, 1, 0) > 0) { n = read(fd, reply + got, 260 - got); ... }
	if (got >= 7 && got >= 6 + (size_t)(reply[4] << 8 | reply[5])) return (int)got - 7;
}
```

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

Free ports (`free_port()`, or `port=0` with \ref mogbus::core::serve "serve()") matter as soon as tests
run in parallel: any two tests may run at once. The `Station` the Python fixture yields holds `ctx`,
`dev`, `process`, `policy`, `server` and the ports, and has `client()` (a new `Client`) and `http(method,
path, body)` (a call to the control API of [step 5](tutorial-faults.md)). The excerpt omits the lines
that also install the post-write hook and the vendor function code of step 4.

## Tests with unittest

pytest is not required. The Python example has two small functions standing in for the code you ship (a
status screen, `read_status()`, and a setpoint raiser, `raise_setpoint()`) in
[`test_pump_station.py`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/python/test_pump_station.py).
`ps` is the `pump_station` module, `self.station` and `self.plc` the station and its client:

```python
def test_setpoint_rules(self):
    self.assertTrue(raise_setpoint(self.plc, 1.0))
    self.assertEqual(read_status(self.plc)["setpoint"], 3.5)
    self.assertFalse(raise_setpoint(self.plc, 10.0))          # exception 03
    self.assertEqual(read_status(self.plc)["setpoint"], 3.5)  # unchanged
    self.assertEqual(self.station.policy.refused, 1)
```

and one that checks the client survives an injected exception 06 from the control API:

```python
def test_client_survives_an_injected_exception(self):
    self.station.http("PUT", "/api/devices/1/faults/exception", {"exception_code": 6})
    with self.assertRaises(ModbusException) as raised:
        read_status(self.plc)
    self.assertEqual(raised.exception.code, 6)
    self.station.http("DELETE", "/api/devices/1/faults/exception")
    self.assertEqual(read_status(self.plc)["site"], "NORTH-01")
```

```bash
make python-examples      # builds libmogbus.so, runs pump_station.py and these tests
```

```text
.......
----------------------------------------------------------------------
Ran 8 tests in 0.183s

OK
```

The C program is checked the same way (`make lib-check` runs `--self-test`, which prints one `ok` or
`FAIL` line per check and exits 0 when all pass). The same files are the pytest fixtures and tests in
`conftest.py` and `test_station.py`; pytest is not a dependency of mogbus, so CI does not run them.
Fixtures, pytest, and habits that keep tests reliable (one context per test, simulated time as a
function, `probe()` for readiness, assertions on the audit trail and the metrics) are in
[Testing with mogbus in Python](../python/python-testing.md).

## Next

- [Step 8: Stop it and ship it](tutorial-ship.md)
