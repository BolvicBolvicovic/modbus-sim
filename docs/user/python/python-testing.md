# Testing with pytest and the control API

A pytest fixture around `mogbus.serve()`, the runnable examples, and how another process changes a running server over HTTP.

## pytest

pytest is not required; a fixture is a few lines:

```python
import mogbus, pytest

@pytest.fixture
def meter():
    with mogbus.serve(config="profiles/power-meter.scm", port=0) as srv:
        yield srv

def test_voltage(meter):
    meter.device(1).set_value("input", 0, 230.0)
    # connect your client to 127.0.0.1:meter.port
```

## Runtime control over HTTP

The Python calls change a device from inside the test process. A server also has an opt-in HTTP
control API that other processes can use to read and write values and inject faults. Enable it with
`http_bind_address`, `http_port` and `http_control=True` in `ServerOptions`, or:

```python
mogbus.serve(..., http_bind_address="127.0.0.1", http_port=9102, http_control=True)
```

See the [runtime control API](../observability/api/control-api.md). It has no authentication, so keep
it on loopback.

## Examples

[`examples/python/`](https://github.com/BolvicBolvicovic/mogbus/tree/main/examples/python) has
`embed.py`, the Python version of `examples/embed_tcp.c` (device built in code, background server, raw
MBAP client, `set_value()` while serving; CI runs it with `make python-examples`), and a pytest
example (`conftest.py` fixture plus `test_device.py`, which needs pytest). Its README says how to run
them.

## See also

- [Getting started with Python](python-start.md)
- [Threading and logging](python-threading.md)
