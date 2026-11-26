# mogbus for Python

`ctypes` bindings for the [mogbus](../README.md) Modbus device simulator. Standard library only,
Python 3.9 or newer, Linux only. See [docs/user/python.md](../docs/user/python.md).

```bash
make python              # builds libmogbus.so and copies it into python/mogbus/
pip install ./python
```

```python
import mogbus

with mogbus.serve(config="profiles/minimal.scm", port=0) as srv:
    print(srv.port, srv.device(1).read("holding", 0, 1))
```

With a config, `serve()` uses what the binary would (its `(server ...)` section, or TCP 1502 without one) and only the keyword arguments you pass override it; `port=0` picks a free port, and without a config you get TCP on a free port.

Without the copied library, set `MOGBUS_LIB=/path/to/libmogbus.so`.
Run the tests with `make python-tests`.
