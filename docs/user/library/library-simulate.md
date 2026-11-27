# Step 3: Simulate the process from the host

Move the tank level, pump, alarms and run time from the program, and see how host writes differ from what a client's write does.

Between steps of the server loop, `simulate()` advances the process by the elapsed time and publishes it. Nothing is computed per request: values change when the host changes them (or, for ramps and clock points, when they are read). The self-test calls `simulate(st, 1.0)` directly to move simulated time without waiting.

## The model

The station is a tank with a constant inflow and a pump that drains it. The pump follows the mode register (`0` off, `1` auto, `2` manual), the setpoint and the run command coil:

- Off: the pump is stopped.
- Manual: the pump follows coil 0, which the client writes.
- Auto: hysteresis around the setpoint, on above setpoint + 0.2 m and off below setpoint - 0.2 m.

The level rises 0.02 m/s and falls 0.05 m/s while the pump runs. The high alarm trips above 4.0 m and the low alarm below 0.5 m.

## Reading what the client wrote

Clients drive the model through the setpoint, the mode and the run command, so the host reads them back at the start of each tick. @ref mogbus_device_get_value "mogbus_device_get_value()" returns the decoded value of the point that starts at an address (a float32 comes back as the number, not two registers); @ref mogbus_device_read "mogbus_device_read()" returns raw 16-bit registers (bits as 0 or 1) and is exact for any type:

```c
mogbus_device_get_value(dev, MOGBUS_HOLDING, HOLD_MODE, &mode);
mogbus_device_get_value(dev, MOGBUS_HOLDING, HOLD_SETPOINT, &setpoint);
mogbus_device_read(dev, MOGBUS_COIL, COIL_RUN, 1, &coil);
```

Both take the point's first address. Reads advance ramps to the current time first. Reading is a host call, so it never invokes the access hook. The self-test checks that a client write is visible to the host:

```text
  ok   FC 16 setpoint 3.0 m is accepted
  ok   the host reads the new setpoint back
```

## Publishing the process: set_value

Sensor readings are what clients must not write, so the host sets them with @ref mogbus_device_set_value "mogbus_device_set_value()":

```c
mogbus_device_set_value(dev, MOGBUS_INPUT, IN_LEVEL, st->level);
mogbus_device_set_value(dev, MOGBUS_INPUT, IN_CURRENT, pump ? 85 : 0);
mogbus_device_set_value(dev, MOGBUS_INPUT, IN_RUN_SECONDS, (double)(uint32_t)st->run_s);
mogbus_device_set_value(dev, MOGBUS_DISCRETE, DISC_HIGH, st->high);
```

It is the host side of a point, like a machine output: it works on every table (inputs and discrete inputs cannot be written by a client at all, and read-only points too), but the point becomes host-owned, so a ramp stops and any client write to it gets exception 02. Values are decoded numbers, so a float32, a u32 and a bit are set the same way; for bit points nonzero means on.

## Acting like a client: write

The run command coil is the client's input in manual mode, so the host must not claim it with `set_value`. Outside manual mode, though, the real PLC would drive it, so the host mirrors the pump into the coil with @ref mogbus_device_write "mogbus_device_write()":

```c
if (mode != MODE_MANUAL && coil != pump)
{
	uint16_t v = pump;

	mogbus_device_write(dev, MOGBUS_COIL, COIL_RUN, 1, &v);
}
```

`mogbus_device_write` is what a client write does: it addresses raw registers, works on every table and on read-only points (the host plays the physical side), turns a ramp static, runs machine on-write handlers, and fails on points a machine drives or that `set_value` claimed. It leaves the point writable by clients, which is why the coil keeps working in manual mode. Like every host call it skips the access hook.

| | `mogbus_device_set_value` | `mogbus_device_write` |
|---|---|---|
| Addressing | a point's first address, decoded number | raw registers |
| Read-only point / input / discrete | works | works |
| Afterwards a client write | exception 02 (host-owned) | allowed as before |
| Ramp | stops | becomes static |
| Machines | like a machine output | runs on-write handlers |

## Strings

@ref mogbus_device_get_string "mogbus_device_get_string()" copies a string point's text up to the first NUL into your buffer (it fails when the buffer cannot hold the text plus the terminator); @ref mogbus_device_set_string "mogbus_device_set_string()" replaces it, NUL padded. Unlike `set_value` it does not make the point host-owned: clients can still write the registers, and a restart restores the configured text. `get_value` and `set_value` return false on string points.

```text
  ok   the host reads the site name with mogbus_device_get_string
```

## Putting it together

Because the same loop serves clients and moves the process, a scenario can be scripted from the host. The self-test switches the station off, advances 100 simulated seconds and checks what a client sees:

```text
  ok   FC 06 switches the station off
  ok   100 s later the high-level alarm is set
  ok   FC 04 reads a level above 4 m
```

Fault injection (delays, dropped replies, exceptions, restarts) comes from a config; load one with @ref mogbus_load_config "mogbus_load_config()" when the harness needs an unreliable device.

## Next

- [Step 4: Guard clients and keep records](library-hooks.md)
