# Using mogbus from Python

Build simulated Modbus devices in Python, serve them from a background thread and test your own client code against them, with a `ctypes` package over `libmogbus.so`.

You maintain a SCADA screen, a data logger or some control code that talks Modbus to a device, and the
real device is not on your desk. The `mogbus` Python package builds the device, serves it, lets the
host program move its values, enforces the rules of the real controller, shows what your client does
and turns it all into automated tests. The [pumping station tutorial](../tutorial/tutorial-start.md)
walks through one realistic program in config, C and Python; the complete Python program is
[`examples/python/pump_station.py`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/python/pump_station.py),
with a small Modbus TCP client,
[`modbus_tcp.py`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/python/modbus_tcp.py),
that stands in for your own client code.

The names of the classes and methods link to the \ref core.py "Python API" reference, which is
generated from the docstrings of the package; these pages do not repeat it.

## Pages

- [Devices and points](python-device.md): points of every kind, strings, ramps, file records, a FIFO, a device from a config file.
- [Serving](python-serve.md): server options and transports, `serve`, probing.
- [Host writes](python-simulate.md): host writes against client writes.
- [Hooks](python-hooks.md): the access hook, post-write hook, a vendor function code, frame and connection hooks.
- [Logs, control, threads and lifetimes](python-lifetimes.md)
- [Testing](python-testing.md): unittest and pytest fixtures.
- [The Python client](python-client.md) and [asyncio](python-asyncio.md).

## Install

The package is pure `ctypes` over `libmogbus.so`: no compiled extension, Python 3.9 or newer, Linux
only (on Windows use [WSL2](../windows/windows-wsl2.md)). From a checkout:

```bash
make python              # builds libmogbus.so and copies it into python/mogbus/
pip install ./python     # the wheel contains the library
```

or point the package at any library build without installing it:

```bash
make lib
MOGBUS_LIB=$PWD/libmogbus.so PYTHONPATH=python python3 -c "import mogbus; print(mogbus.library_version())"
```

```text
0.0.1
```

\ref core.py "The API overview" lists where the package looks for the library and the limits it
inherits from the C library (16 devices and 8 servers per context).

## Run it

From a checkout, with the package built as above:

```bash
PYTHONPATH=python python3 examples/python/pump_station.py
```

The program runs every step of the tutorial in order, prints what they show and exits with status 0
when all its checks pass (each prints an `ok` line).

## See also

- \ref core.py "Python API reference"
- [Pumping station tutorial](../tutorial/tutorial-start.md)
- [Embedding mogbus in C](../library/library-start.md)
- [Windows route](../windows/windows-route.md)
