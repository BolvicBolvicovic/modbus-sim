# Get started

Build mogbus, start a simulated device, read a register from it and change its config. About five minutes.

You describe a device in a small text file. A config file is a short list of `(name value)` entries,
nested where it helps. The syntax is inspired by Scheme, but it is plain data: there is no code to
write, only settings to fill in. mogbus then pretends to be that device on the network or on a serial
line, and you point your SCADA, PLC, gateway or test script at it.

## 1. Build it

You need a C compiler, `make`, and (for TLS) the OpenSSL development files. The `vlib/` directory is a
git submodule, so clone with `--recurse-submodules`:

```bash
git clone --recurse-submodules https://github.com/BolvicBolvicovic/mogbus.git
cd mogbus
make -j$(nproc)        # builds ./mogbus
```

If you cloned without the submodule, run `git submodule update --init` (`make` stops with a clear
error otherwise). Without OpenSSL, `make WITH_TLS=0` builds everything except TLS. See
[Build from source](building.md) for all options.

On Windows, see [Running mogbus on Windows](windows/windows-route.md). To skip the build, use the
[Docker image](deployment/docker.md).

## 2. Run a profile

A profile is a ready-made config for one kind of device. Start the smallest one:

```bash
./mogbus profiles/minimal.scm
```

It serves Modbus TCP on `127.0.0.1:1502`. Leave it running.

## 3. Read a register

In another terminal, use the small client that ships with mogbus:

```bash
./tools/modbus_probe.py holding 0 2    # -> [42, <a value that moves>]
```

Holding register 0 always returns 42; register 1 counts 0, 10, 20 ... 100 and starts again, one step
per second. Run the command again and the second value changes.

`tools/modbus_probe.py` is a small Modbus TCP client. Any other client works too: point it at
`127.0.0.1:1502`, unit id 1. The RTU, ASCII and UDP modes need a client that speaks their framing.

## 4. Change the config

Create `my-device.scm`:

```scheme
(server (mode tcp) (port 1502))

(device
  (name "My first device")
  (unit-id 1)
  (holding 0 (static 42))                                                      ; never changes
  (holding 1 (ramp (from 0) (to 100) (step 10) (every-ms 1000) (mode wrap)))   ; counts up
  (input 0 (type float32) (static 21.5))                                       ; two registers
  (coil 0 (static 1)))
```

Check it without starting the server:

```bash
./mogbus --check=verbose my-device.scm
```

`--check=verbose` prints the same summary as a normal start (plain `--check` prints nothing unless the file is wrong) (transport, devices, point counts, faults) and
validates the server settings, including TLS files, without opening any socket or device. Then run
it and read the new points:

```bash
./mogbus my-device.scm

# in the other terminal:
./tools/modbus_probe.py input 0 2 --as float32     # -> [21.5]
./tools/modbus_probe.py coil 0 1                   # -> [1]
./tools/modbus_probe.py write-reg 0 1234           # write a holding register
./tools/modbus_probe.py holding 0 1                # -> [1234]
```

Only the points you configure exist. A client that reads any other address gets Modbus exception 02.

## Where to go next

- Follow the [pumping station tutorial](tutorial/tutorial-start.md): a full simulated device with behaviours, a
  machine, faults, the web UI and the control API.
- Copy a [profile](profiles/using-profiles.md) that is close to your device, or
  [import a register map](import/import-overview.md).
- Add behaviour and faults: the [config reference](config/format.md).
- Serve another transport: [Transports](transports/transports.md).
- Run it in an automated test setup or a container: [Docker](deployment/docker.md), [Python](python/python-start.md).
- Look at what a client sends: [Logs](observability/logs.md), or run `./mogbus -v my-device.scm`.
