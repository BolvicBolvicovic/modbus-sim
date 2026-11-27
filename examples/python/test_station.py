"""The same station as test_pump_station.py, as pytest tests with the `station` fixture (conftest.py)."""
import pytest

import pump_station as ps
from modbus_tcp import ModbusException


def test_status_after_a_minute(station):
    station.process.tick(60.0)
    with station.client() as plc:
        assert plc.read_string(ps.SITE_NAME, 8) == "NORTH-01"
        assert plc.read_f32(4, ps.LEVEL) == pytest.approx(2.33, abs=0.01)


@pytest.mark.parametrize("setpoint", [0.0, 0.4, 4.6, 9.0, float("nan")])
def test_bad_setpoints_are_refused(station, setpoint):
    with station.client() as plc, pytest.raises(ModbusException) as raised:
        plc.write_f32(ps.SETPOINT, setpoint)
    assert raised.value.code == 3
    assert station.dev.get_value("holding", ps.SETPOINT) == 2.5
