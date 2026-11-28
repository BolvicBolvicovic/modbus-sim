# Step 3: Make it behave

> **Summary:** Make the station move like a plant: the tank fills, the pump runs in auto or manual mode, the alarms follow the level and the motor current follows the pump, described in the config or computed by your own program.

So far every value is fixed. The *process* behind the tables (water in, water out, a motor drawing
current) is yours to model, and the routes do it differently:

- **Scheme / Bash**: you describe the process in the config file and mogbus computes it, at the moment a request arrives. Nothing runs between requests.
- **C and Python**: your program computes the process and writes the measurements into the device with `set_value`, once per pass of its own loop. This is how a host program plays the part of the physical plant.

Both routes model the same plant:

- The tank gains 2 cm per second while the pump is stopped and loses 4 cm per second while it runs.
- Mode 0 (off): the pump is stopped. Mode 1 (auto): it starts when the level is more than 0.3 m above the setpoint and stops when the level is more than 0.3 m below it. Mode 2 (manual): it follows the run command coil.
- The high-level alarm is on above 4 m, the low-level alarm below 1 m.
- The motor current is 12 A while the pump runs.

## The model

Save this as `pump-station.scm`. It is the device of step 1 with live values and two *machines*, small
state machines that own some points. The C and Python programs do the same work in code.

```scheme
; pump-station.scm - the pump station of step 1 with live values and a process behind them.

(server (mode tcp) (port 1502) (seed 7))

(device
  (name "Pump station")
  (unit-id 1)

  ; Holding registers, coil and discrete inputs: as in step 1.
  (holding 0 (type float32) (static 2.5))      ; level setpoint in metres
  (holding 2 (type u16) (static 1))            ; mode: 0 off, 1 auto, 2 manual
  (holding 10 (type string) (length 8) (access read-only) (static "NORTH-01"))
  (coil 0)                                     ; pump run command (manual mode)
  (discrete 0)                                 ; high-level alarm, set by the alarms machine
  (discrete 1)                                 ; low-level alarm, set by the alarms machine
  (discrete 2)                                 ; pump running, set by the pump machine

  ; Tank level in metres and pump run time in seconds: the pump machine below writes them.
  (input 0 (type float32) (static 2.5))
  (input 10 (type u32) (static 0))

  ; Outflow in m3/h: 40 plus or minus 6 over a two-minute period, with some random noise.
  (input 2 (type float32)
    (sine (offset 40) (amplitude 6) (period-ms 120000))
    (noise (amplitude 0.8) (every-ms 500) (distribution gaussian)))

  ; Motor current in tenths of an ampere: heads for 12.0 A (120) while "pump running" is on
  ; and back to 0 when it is off, closing 63% of the gap every 1.5 seconds.
  (input 4 (type u16)
    (lag (target (discrete 2)) (tau-ms 1500) (gain 120))
    (noise (amplitude 2) (every-ms 250)))

  ; Wall clock, seconds since 1970.
  (input 20 (type u32) (clock unix))

  ; The pump. The tank gains 2 cm per second while the pump is stopped and loses 4 cm per
  ; second while it runs. (elapsed-ms) is the time since the machine last ran.
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

    ; Start: auto mode (holding 2 = 1) and the level above setpoint + 0.3 m, or manual mode
    ; (holding 2 = 2) and the run command coil on.
    (transition stopped running
      (when (or (and (= (holding 2) 1) (> (input 0) (+ (holding 0) 0.3)))
                (and (= (holding 2) 2) (coil 0)))))
    ; Stop: mode off, or auto mode and the level below setpoint - 0.3 m, or manual mode and
    ; the run command off.
    (transition running stopped
      (when (or (= (holding 2) 0)
                (and (= (holding 2) 1) (< (input 0) (- (holding 0) 0.3)))
                (and (= (holding 2) 2) (not (coil 0)))))))

  ; The alarms: high above 4 m, low below 1 m.
  (machine alarms
    (initial watching)
    (state watching
      (on-tick (set! (discrete 0) (> (input 0) 4))
               (set! (discrete 1) (< (input 0) 1))))))
```

```c
/* simulate.c - serve the pump station and run its process from the host program. */
#include <stdbool.h>
#include <stdio.h>
#include "mogbus.h"

int
main(void)
{
	mogbus_t* ctx = NULL;
	mogbus_server_t* server = NULL;
	mogbus_device_t* dev = NULL;

	if (mogbus_create(&ctx) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(NULL));
		return 1;
	}
	if (mogbus_load_config(ctx, "station.scm") != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(ctx));
		return 1;
	}
	dev = mogbus_find_device(ctx, 1); /* the device with unit id 1 */

	mogbus_server_options_t options = mogbus_server_options_defaults();

	if (mogbus_server_create(ctx, &options, &server) != MOGBUS_OK)
	{
		fprintf(stderr, "error: %s\n", mogbus_last_error(ctx));
		return 1;
	}

	/* The state of the process, kept by the program. */
	double level = 2.5;      /* tank level in metres */
	bool pump = false;       /* true while the pump runs */
	double run_seconds = 0;  /* how long the pump has run */
	uint64_t last_ms = mogbus_now_ms();

	while (mogbus_server_step(server, 100) == MOGBUS_STEP_CONTINUE)
	{
		/* Seconds since the previous pass of this loop. */
		uint64_t now_ms = mogbus_now_ms();
		double seconds = (double)(now_ms - last_ms) / 1000.0;

		last_ms = now_ms;

		/* What a client may have written: the setpoint (holding 0), the mode (holding 2)
		 * and the run command (coil 0). The calls below can only fail when no point starts
		 * at the address, and the points of step 1 are there, so the results are not checked. */
		double setpoint = 0;
		double mode = 0;
		double run_command = 0;

		mogbus_device_get_value(dev, MOGBUS_HOLDING, 0, &setpoint);
		mogbus_device_get_value(dev, MOGBUS_HOLDING, 2, &mode);
		mogbus_device_get_value(dev, MOGBUS_COIL, 0, &run_command);

		/* Does the pump run? Mode 0 is off, 1 is auto (start 0.3 m above the setpoint, stop
		 * 0.3 m below it), 2 is manual (follow the run command). */
		if (mode == 0)
			pump = false;
		if (mode == 1 && level > setpoint + 0.3)
			pump = true;
		if (mode == 1 && level < setpoint - 0.3)
			pump = false;
		if (mode == 2)
			pump = run_command != 0;

		/* The tank gains 2 cm per second, and loses 4 cm per second while the pump runs. */
		level += (pump ? -0.04 : 0.02) * seconds;
		if (level < 0)
			level = 0;
		if (level > 5)
			level = 5;
		if (pump)
			run_seconds += seconds;

		/* Publish what a sensor would measure. Clients can read these but not write them. */
		mogbus_device_set_value(dev, MOGBUS_INPUT, 0, level);                    /* level, m */
		mogbus_device_set_value(dev, MOGBUS_INPUT, 4, pump ? 120 : 0);           /* motor 12.0 A */
		mogbus_device_set_value(dev, MOGBUS_INPUT, 10, run_seconds);             /* run time, s */
		mogbus_device_set_value(dev, MOGBUS_DISCRETE, 0, level > 4);             /* high alarm */
		mogbus_device_set_value(dev, MOGBUS_DISCRETE, 1, level < 1);             /* low alarm */
		mogbus_device_set_value(dev, MOGBUS_DISCRETE, 2, pump);                  /* pump running */
	}

	mogbus_destroy(ctx);
	return 0;
}
```

```python
# simulate.py - serve the pump station and run its process from the script.
import time
import mogbus

with mogbus.Context() as ctx:
    ctx.load_config("station.scm")
    dev = ctx.find_device(1)  # the device with unit id 1

    level = 2.5               # tank level in metres
    pump = False              # True while the pump runs
    run_seconds = 0.0         # how long the pump has run
    last = time.monotonic()

    try:
        with ctx.serve(mogbus.ServerOptions(port=1502)).start():  # serves on a background thread
            while True:
                time.sleep(0.1)
                now = time.monotonic()
                seconds = now - last   # seconds since the previous pass of this loop
                last = now

                # What a client may have written: the setpoint (holding 0), the mode (holding 2)
                # and the run command (coil 0).
                setpoint = dev.get_value("holding", 0)
                mode = dev.get_value("holding", 2)
                run_command = dev.get_value("coil", 0)

                # Does the pump run? Mode 0 is off, 1 is auto (start 0.3 m above the setpoint,
                # stop 0.3 m below it), 2 is manual (follow the run command).
                if mode == 0:
                    pump = False
                if mode == 1 and level > setpoint + 0.3:
                    pump = True
                if mode == 1 and level < setpoint - 0.3:
                    pump = False
                if mode == 2:
                    pump = run_command != 0

                # The tank gains 2 cm per second, and loses 4 cm per second while the pump runs.
                level += (-0.04 if pump else 0.02) * seconds
                level = min(max(level, 0.0), 5.0)
                if pump:
                    run_seconds += seconds

                # Publish what a sensor would measure. Clients can read these but not write them.
                dev.set_value("input", 0, level)                  # level, m
                dev.set_value("input", 4, 120 if pump else 0)     # motor current, 12.0 A
                dev.set_value("input", 10, run_seconds)           # run time, s
                dev.set_value("discrete", 0, level > 4)           # high alarm
                dev.set_value("discrete", 1, level < 1)           # low alarm
                dev.set_value("discrete", 2, pump)                # pump running
    except KeyboardInterrupt:
        pass                  # Ctrl-C ends the script; the with blocks have closed everything
```

- **Scheme / Bash.** `sine` and `noise` make the outflow wander around 40 m3/h, and `lag` makes the motor current follow the "pump running" bit like a real motor ([`sine`](../config/behaviours/sine.md), [`noise`](../config/behaviours/noise.md), [`lag`](../config/behaviours/lag.md), [`clock`](../config/behaviours/clock.md)). `(seed 7)` makes the random parts repeat on every run ([`seed`](../config/server/seed.md)). The `pump` machine is the control logic: its `transition` lines read the mode, the setpoint and the coil that a client can write, and `elapsed-ms` is the time since the machine last ran, so the result does not depend on how often a client polls. See [machines](../config/machines/index.md) and [expressions](../config/machines/expressions.md).
- **C.** The program loads `station.scm` of step 1 (without machines) and does the computing itself, between two calls of @ref mogbus_server_step "mogbus_server_step()". It reads what a client may have written with @ref mogbus_device_get_value "mogbus_device_get_value()" and publishes the measurements with @ref mogbus_device_set_value "mogbus_device_set_value()". Both take the table and the first address of the point.
- **Python.** The same, with \ref mogbus::core::Device::get_value "Device.get_value()" and \ref mogbus::core::Device::set_value "Device.set_value()". The server runs on its own thread, and the loop of the script is the plant. `Ctrl-C` ends the script.

If you load `pump-station.scm` with `mogbus_load_config()` / `Context.load_config()` in a C or Python
program instead, the machines run there as well and you do not need the loop: use whichever you prefer,
not both.

## Watch it run

Start the server of your route: `./mogbus pump-station.scm`, `./simulate` (build `simulate.c` as in the
[start page](tutorial-start.md#before-you-start)) or `PYTHONPATH=python python3 simulate.py`. Then watch
the tank level from a second terminal. `--watch 1` repeats the read every second:

```bash
./tools/modbus_probe.py --watch 1 input 0 2 --as float32
```

```text
[2.5212600231170654]
[2.5412800312042236]
[2.5612800121307373]
[2.581279993057251]
```

The level climbs 0.02 m per second. In auto mode it passes 2.8 m, the setpoint plus 0.3 m, after about 15
seconds: the pump starts and the level falls 0.04 m per second until it reaches 2.2 m, where the pump stops
and the level climbs again. The other values:

```bash
./tools/modbus_probe.py discrete 0 3                 # high alarm, low alarm, pump running
./tools/modbus_probe.py input 2 2 --as float32       # outflow, m3/h (Scheme / Bash route only)
./tools/modbus_probe.py input 20 2 --as u32          # wall clock (Scheme / Bash route only)
```

The outflow and the wall clock are moved by the config of the Scheme / Bash route; in the C and Python
programs of this step they keep the fixed values of step 1 (the ramp and the wall clock of step 1 still
move).

## Start the pump by hand

Switch to manual mode (mode 2) and set the run command coil. The pump starts and the motor current climbs
towards 12 A, which is 120 in tenths of an ampere:

```bash
./tools/modbus_probe.py write-reg 2 2        # mode 2 = manual
./tools/modbus_probe.py write-coil 0 on      # run command
./tools/modbus_probe.py discrete 2 1         # pump running
./tools/modbus_probe.py --watch 1 input 4 1  # motor current, 0.1 A
```

```text
ok
ok
[1]
[0]
[58]
[86]
[102]
[112]
[117]
```

The two `ok` are the writes and `[1]` is "pump running". The motor current of the Scheme / Bash route
follows the pump with the `lag` of the config, over a few seconds. The C and Python programs write 120 as
soon as the pump runs, so they print `[120]` at once.

Writes a real device would refuse are refused here too: a write to the read-only site name, to an address
with no point, or to the tank level, which the machine (or the host program) owns, answers `error:
exception 0x02: illegal data address`. Writing a point that moves by itself with a ramp, sine or clock
freezes it at the written value, which is how a test changes a sensor ([client
writes](../config/behaviours/client-writes.md)).

## Who owns a point

A point that a machine or the host program writes belongs to it, and a client write to it fails with
exception 02: the simulation stays in control. In C and Python this is the difference between the two ways
to change a value from the host:

| | `set_value` | `write` |
|---|---|---|
| Address | first address of a point, a decoded number | raw registers |
| Works on read-only points, inputs, discrete inputs | yes | yes |
| Afterwards a client write to the point | refused with exception 02 | allowed as before |
| A ramp | stops | becomes static |

Use `set_value` (@ref mogbus_device_set_value "mogbus_device_set_value()", \ref mogbus::core::Device::set_value "Device.set_value()") for what a sensor would
measure, and `write` (@ref mogbus_device_write "mogbus_device_write()", \ref mogbus::core::Device::write "Device.write()") when the host should act like a client. Details: [the host
side in C](../library/library-simulate.md) and [in Python](../python/python-simulate.md).

A config can also run statements when a client reads or writes a point, for example a counter that
increases at every read: [point actions](../config/actions/index.md).

## Next

- [Step 4: Guard clients and keep records](tutorial-guard.md)
