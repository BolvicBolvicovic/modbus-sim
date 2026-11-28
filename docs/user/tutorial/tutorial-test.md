# Step 7: Test your client

> **Summary:** Use the station as a test fixture: a fresh simulated plant for every test, a fault or a sensor value set by the test, and your own client code run against it in CI.

The point of all this is to test *your* code: the SCADA screen, the data logger. The tests below check one
small function that reads the tank level, standing in for your own code. The routes differ in who starts
the station:

- **Scheme / Bash**: a server process, started before the test and stopped after it. The script waits with `mogbus --health` until the station answers.
- **C**: the same, with a test program written with the C client. The test program is a client only; the station is the `mogbus` program or your own server program.
- **Python**: a test starts its own station inside the test process, on a free port, with `mogbus.serve()`; nothing is left running when the test ends.

## A test in C, run by a script

`test_level.c` is the client code under test (`read_level()`) and the test itself. The script starts the
station, waits until it answers, runs the test and stops the station. Its exit status is the status of the
test, so a CI system can use it as it is.

```c
/* test_level.c - a test for the code that reads the tank level; the exit status is the result. */
#include <math.h>
#include <stdio.h>
#include "mogbus.h"

/* The code under test: read the tank level (input registers 0 and 1, a float32) in metres. */
static mogbus_result_t
read_level(mogbus_client_t* plc, double* level)
{
	uint16_t registers[2] = { 0, 0 };
	mogbus_result_t result = mogbus_client_read_input_registers(plc, 1, 0, 2, registers);

	if (result != MOGBUS_OK)
		return result;
	return mogbus_registers_to_value(registers, MOGBUS_TYPE_F32, MOGBUS_WORDS_HIGH_FIRST,
					 MOGBUS_BYTES_BIG, level);
}

int
main(void)
{
	mogbus_client_t* plc = NULL;
	mogbus_client_options_t options = mogbus_client_options_defaults(); /* TCP to 127.0.0.1:1502 */
	double level = 0;

	if (mogbus_client_open(&options, &plc) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(NULL));
		return 1;
	}
	if (read_level(plc, &level) != MOGBUS_OK)
	{
		fprintf(stderr, "FAIL: %s\n", mogbus_client_last_error(plc));
		return 1;
	}

	/* station.scm starts the tank at 2.5 m. */
	if (fabs(level - 2.5) > 0.001)
	{
		fprintf(stderr, "FAIL: expected a level of 2.5 m, read %.3f m\n", level);
		return 1;
	}
	printf("PASS: level %.1f m\n", level);

	mogbus_client_close(plc);
	return 0;
}
```

The script, saved as `run_tests.sh`:

```bash
#!/bin/sh
# run_tests.sh - start the station, run the test, stop the station.

./mogbus station.scm &       # the station, in the background
station=$!

until ./mogbus --health station.scm; do   # wait until it answers a Modbus request
    sleep 0.2
done

./test_level                 # your test: for the C route, test_level.c
status=$?                    # 0 means the test passed

kill $station                # stop the station
exit $status
```

Build the test as `test_level` (see the [start page](tutorial-start.md#before-you-start)) and run the script with
`sh run_tests.sh`, from the repository root, where `./mogbus` and `station.scm` are:

```text
unhealthy: connect 127.0.0.1:1502: Connection refused
PASS: level 2.5 m
```

The first line is the script waiting for the server to start: `--health` fails until the station answers.
The station also prints its log lines. A script for the Scheme / Bash route is the same with your own
client in place of `./test_level`, for example `./tools/modbus_probe.py input 0 2 --as float32`.

## A test in Python

Python builds the station inside the test. `mogbus.serve()` loads a config, serves it on a background
thread and stops it when the `with` block ends; `port=0` means "pick a free port", so tests can run in
parallel. The station of a test can also come from text, with a fault in it:

```python
# test_station.py - tests for a small piece of client code, run against the pump station.
import contextlib
import unittest

import mogbus


def read_level(plc):
    """The code under test: read the tank level (input registers 0 and 1, a float32) in metres."""
    registers = plc.read_input_registers(0, 2)
    return mogbus.registers_to_value(registers, "f32")


class StationTest(unittest.TestCase):
    def setUp(self):
        # A fresh station for every test. port=0 picks a free port, so tests can run in
        # parallel; the station is stopped when the test ends.
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        self.station = stack.enter_context(mogbus.serve(config="station.scm", port=0))
        self.plc = stack.enter_context(mogbus.Client(port=self.station.port, unit=1))

    def test_reads_the_level(self):
        # Play the sensor: set the tank level to 3.25 m on the station's side ...
        self.station.device(1).set_value("input", 0, 3.25)
        # ... and check that the client code sees it.
        self.assertEqual(read_level(self.plc), 3.25)

    def test_busy_station(self):
        # A station that answers every request with exception 06 (server device busy).
        config = """
        (device (unit-id 1)
          (input 0 (type float32) (static 2.5))
          (fault (kind exception) (code 6) (percent 100)))
        """
        with mogbus.serve(config_text=config, port=0) as busy:
            with mogbus.Client(port=busy.port, unit=1) as plc:
                with self.assertRaises(mogbus.ModbusException) as raised:
                    read_level(plc)
                self.assertEqual(raised.exception.code, 6)


if __name__ == "__main__":
    unittest.main()
```

Run it with `python3 -m unittest test_station -v` (with `PYTHONPATH=python` as before). It needs no
test framework other than the `unittest` of the standard library. After the log lines of the
stations it ends with:

```text
Ran 2 tests in 0.030s

OK
```

The test sets the level with `set_value` on the device of the running station: it is the same call a
program of step 3 uses to play the sensor. Faults come from the config (as in `test_busy_station`) or from
the control API of [step 5](tutorial-faults.md). Fixtures for pytest, the habits that keep tests reliable (one
context per test, simulated time as a function, `probe()` for readiness, assertions on the metrics of
[step 6](tutorial-observe.md)) are in [Testing with mogbus in Python](../python/python-testing.md).

## Next

- [Step 8: Stop it and ship it](tutorial-ship.md)
