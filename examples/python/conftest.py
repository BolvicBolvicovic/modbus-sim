"""pytest fixture: a served meter per test. Run with
   MOGBUS_LIB=$PWD/libmogbus.so PYTHONPATH=python python3 -m pytest examples/python
"""
import socket

import pytest

import mogbus

CONFIG = """
(device (name "meter") (unit-id 1)
  (holding 0 (static 42))
  (input 0 (type float32) (static 230.0)))
"""


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture
def meter():
    # http_control lets a test inject faults over loopback HTTP (docs/user/observability/api/control-api.md).
    http_port = free_port()
    with mogbus.serve(config_text=CONFIG, port=0, http_bind_address="127.0.0.1",
                      http_port=http_port, http_control=True) as srv:
        srv.http_port = http_port
        yield srv


@pytest.fixture
def station():
    """The pump station of pump_station.py: served on free ports, closed after the test."""
    import pump_station
    mogbus.set_log_level("off")
    with pump_station.running_station() as st:
        yield st
    mogbus.set_log_level("info")
