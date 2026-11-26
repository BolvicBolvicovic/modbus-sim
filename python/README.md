# mogbus for Python

`ctypes` bindings for the [mogbus](../README.md) Modbus device simulator. Standard library only,
Python 3.9 or newer, Linux only. See [docs/user/python.md](../docs/user/python.md).

```bash
make python              # builds libmogbus.so and copies it into python/mogbus/
pip install ./python
```

```python
import mogbus

with mogbus.serve(config="profiles/minimal.scm") as srv:
    print(srv.port, srv.device(1).read("holding", 0, 1))
```

Without the copied library, set `MOGBUS_LIB=/path/to/libmogbus.so`.
Run the tests with `make python-tests`.
