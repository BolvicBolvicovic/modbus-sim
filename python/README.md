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

`serve()` uses the config's `(server ...)` section; keyword arguments such as `port=0` or
`mode="udp"` override it, and without a `(server ...)` section it serves TCP on a free port.

Without the copied library, set `MOGBUS_LIB=/path/to/libmogbus.so`.
Run the tests with `make python-tests`.
