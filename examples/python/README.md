# Python examples

- `embed.py`: the Python counterpart of `examples/embed_tcp.c`. Builds a device in code, serves
  Modbus TCP on a free port from a background thread, reads it with a raw MBAP client, changes a
  value with `Device.set_value()` while serving, reads again, prints the request metrics, then
  does the same from `profiles/minimal.scm` with `mogbus.serve()`. Exit status 0 means every
  value matched. Standard library and `mogbus` only.
- `conftest.py` + `test_device.py`: a pytest fixture around `mogbus.serve()` and two tests (read a
  configured value; change a value and inject a fault, the latter over the HTTP control API).
  Needs pytest, which is not a project dependency.

From a checkout, at the repository root:

```bash
make python                                    # builds libmogbus.so into python/mogbus/
PYTHONPATH=python python3 examples/python/embed.py
pip install pytest                             # only for the pytest example
PYTHONPATH=python python3 -m pytest examples/python
```

After `pip install ./python` the `PYTHONPATH=python` is not needed. With a library built by `make lib`
instead of `make python`, set `MOGBUS_LIB=$PWD/libmogbus.so`. `make python-examples` runs
`embed.py` that way.
