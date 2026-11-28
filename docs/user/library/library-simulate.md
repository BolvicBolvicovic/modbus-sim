# Host writes in C

Move the tank level, pump, alarms and run time from the program, and see how host writes differ from what a client's write does.

Between steps of the server loop ([server loop](library-serve.md#the-loop)), the program's function `simulate(st, dt)` advances the process by `dt` simulated seconds and publishes the result to the device. `st` is the `struct station` of the tutorial program ([step 1](../tutorial/tutorial-device.md)): it holds the device handle `st->dev` and the process state (`level`, `pump`, `high`, `low`, `run_s`). Nothing is computed per request: values change when the host changes them (or, for ramps and clock points, when they are read). The self-test of the tutorial program calls `simulate(st, 1.0)` directly to move simulated time without waiting.

## Reading what the client wrote

Clients drive the model through the setpoint, the mode and the run command, so the host reads them back at the start of each tick (`dev` is `st->dev`; the three variables are the program's locals `mode`, `setpoint` and `coil`). @ref mogbus_device_get_value "mogbus_device_get_value()" returns the decoded value of the point that starts at an address (a float32 comes back as the number, not two registers); @ref mogbus_device_read "mogbus_device_read()" returns raw 16-bit registers (bits as 0 or 1) and is exact for any type:

```c
mogbus_device_get_value(dev, MOGBUS_HOLDING, HOLD_MODE, &mode);
mogbus_device_get_value(dev, MOGBUS_HOLDING, HOLD_SETPOINT, &setpoint);
mogbus_device_read(dev, MOGBUS_COIL, COIL_RUN, 1, &coil);
```

Both take the point's first address. Reads advance ramps to the current time first. Reading is a host call, so it never invokes the access hook. The self-test writes a setpoint of 3.0 m as a client and checks that the host reads it back:

```text
  ok   FC 16 setpoint 3.0 m is accepted
  ok   the host reads the new setpoint back
```

## Publishing the process: set_value

Sensor readings are what clients must not write, so at the end of `simulate()` the host publishes them with @ref mogbus_device_set_value "mogbus_device_set_value()" (`st->level`, `st->high` and `pump` are the process state computed above; the input and discrete addresses are the constants of step 1):

```c
mogbus_device_set_value(dev, MOGBUS_INPUT, IN_LEVEL, st->level);
mogbus_device_set_value(dev, MOGBUS_INPUT, IN_CURRENT, pump ? 85 : 0);
mogbus_device_set_value(dev, MOGBUS_INPUT, IN_RUN_SECONDS, (double)(uint32_t)st->run_s);
mogbus_device_set_value(dev, MOGBUS_DISCRETE, DISC_HIGH, st->high);
```

It is the host side of a point, like a machine output: it works on every table (inputs and discrete inputs cannot be written by a client at all, and read-only points too), but the point becomes host-owned, so a ramp stops and any client write to it gets exception 02. Values are decoded numbers, so a float32, a u32 and a bit are set the same way; for bit points nonzero means on.

## Acting like a client: write

The run command coil is the client's input in manual mode, so the host must not claim it with `set_value`. Outside manual mode, though, the real PLC would drive it, so the host mirrors the pump into the coil with @ref mogbus_device_write "mogbus_device_write()" (`coil` is the value read at the start of the tick, `pump` the new pump state):

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

@ref mogbus_device_get_string "mogbus_device_get_string()" copies a string point's text up to the first NUL into your buffer (it fails when the buffer cannot hold the text plus the terminator); @ref mogbus_device_set_string "mogbus_device_set_string()" replaces it, NUL padded. Unlike `set_value` it does not make the point host-owned: clients can still write the registers, and a restart restores the configured text. `get_value` and `set_value` fail on string points.

```text
  ok   the host reads the site name with mogbus_device_get_string
```

