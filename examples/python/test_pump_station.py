"""Tests of client code against the simulated pump station (unittest; pytest runs them too).

The "code under test" is `read_status()` and `raise_setpoint()` below: the sort of thing a SCADA
script or data logger does. Each test gets a fresh station on free loopback ports from
pump_station.running_station(); simulated time only moves when the test calls process.tick().

    MOGBUS_LIB=$PWD/libmogbus.so PYTHONPATH=python python3 -m unittest discover -s examples/python -p 'test_pump_station.py'
"""
import struct
import unittest

import mogbus
import pump_station as ps
from modbus_tcp import ModbusException


def setUpModule():
    mogbus.set_log_level("off")      # the library logs to stderr; a test run does not need it


def tearDownModule():
    mogbus.set_log_level("info")


def read_status(plc):
    """The operator screen: everything a client needs, in three requests."""
    return {
        "site": plc.read_string(ps.SITE_NAME, 8),
        "level": round(plc.read_f32(4, ps.LEVEL), 2),
        "setpoint": plc.read_f32(3, ps.SETPOINT),
        "running": bool(plc.discrete(ps.RUNNING)[0]),
        "high": bool(plc.discrete(ps.HIGH_ALARM)[0]),
        "low": bool(plc.discrete(ps.LOW_ALARM)[0]),
    }


def raise_setpoint(plc, step):
    """Raises the setpoint by `step` metres; returns False when the station refuses it."""
    try:
        plc.write_f32(ps.SETPOINT, plc.read_f32(3, ps.SETPOINT) + step)
    except ModbusException as exc:
        if exc.code == 3:
            return False
        raise
    return True


class PumpStationTest(unittest.TestCase):
    def setUp(self):
        manager = ps.running_station()
        self.station = manager.__enter__()
        self.addCleanup(manager.__exit__, None, None, None)
        self.plc = self.station.client()
        self.addCleanup(self.plc.close)

    def run_until(self, condition, step=10.0, limit=2000):
        for _ in range(limit):
            if condition():
                return
            self.station.process.tick(step)
        self.fail("the condition never became true")

    def test_status_screen(self):
        self.assertEqual(read_status(self.plc),
                         {"site": "NORTH-01", "level": 2.0, "setpoint": 2.5,
                          "running": False, "high": False, "low": False})

    def test_auto_mode_holds_the_level_around_the_setpoint(self):
        process = self.station.process
        self.run_until(lambda: process.pump)
        self.assertTrue(read_status(self.plc)["running"])
        self.assertAlmostEqual(self.plc.read_f32(4, ps.OUTFLOW), ps.PUMP_FLOW, places=3)
        self.run_until(lambda: not process.pump)
        status = read_status(self.plc)
        self.assertFalse(status["running"])
        self.assertTrue(2.2 <= status["level"] <= 2.3, status)

    def test_manual_mode_follows_the_run_command(self):
        self.plc.write_register(ps.MODE, ps.MANUAL)
        self.station.process.tick(10.0)
        self.assertFalse(read_status(self.plc)["running"])
        self.plc.write_coil(ps.RUN_COMMAND, True)
        self.station.process.tick(10.0)
        self.assertTrue(read_status(self.plc)["running"])
        self.assertGreater(self.plc.input(ps.CURRENT)[0], 0)

    def test_alarms_event_log_and_fifo(self):
        process = self.station.process
        self.plc.write_register(ps.MODE, ps.MANUAL)
        self.run_until(lambda: process.high)
        self.assertTrue(read_status(self.plc)["high"])
        self.plc.write_coil(ps.RUN_COMMAND, True)
        self.run_until(lambda: process.low)
        self.assertTrue(read_status(self.plc)["low"])
        self.assertEqual(self.plc.fifo(ps.ALARM_FIFO), [ps.EV_HIGH_ON, ps.EV_LOW_ON])
        log = self.plc.file_records(ps.EVENT_FILE, 0, ps.EVENT_RECORDS)
        self.assertEqual(log[-3:], [ps.EV_PUMP_ON, ps.EV_HIGH_OFF, ps.EV_LOW_ON])

    def test_setpoint_rules(self):
        self.assertTrue(raise_setpoint(self.plc, 1.0))
        self.assertEqual(read_status(self.plc)["setpoint"], 3.5)
        self.assertFalse(raise_setpoint(self.plc, 10.0))          # exception 03
        self.assertEqual(read_status(self.plc)["setpoint"], 3.5)  # unchanged
        self.assertEqual(self.station.policy.refused, 1)
        self.assertEqual([entry[2] for entry in self.station.policy.audit], [ps.SETPOINT])

    def test_status_block_is_computed_per_request(self):
        def status():
            reply = self.plc.call(struct.pack(">BB", ps.FC_STATUS_BLOCK, 1))
            return struct.unpack(">BBBHHH", reply[1:])

        self.assertEqual(status(), (1, ps.AUTO, 0, 200, 0, 0))
        self.run_until(lambda: self.station.process.pump)
        block_id, mode, flags, level_cm, outflow, refused = status()
        self.assertEqual((mode, flags & 4, outflow), (ps.AUTO, 4, int(ps.PUMP_FLOW * 10)))
        self.assertEqual(level_cm, round(self.plc.read_f32(4, ps.LEVEL) * 100))
        self.assertFalse(raise_setpoint(self.plc, 10.0))
        self.assertEqual(status()[-1], 1)                       # the hook's refusal count
        with self.assertRaises(ModbusException) as raised:
            self.plc.call(struct.pack(">BB", ps.FC_STATUS_BLOCK, 2))
        self.assertEqual(raised.exception.code, 3)

    def test_site_name_is_read_only(self):
        with self.assertRaises(ModbusException) as raised:
            self.plc.write_registers(ps.SITE_NAME, [0x4B4F])
        self.assertEqual(raised.exception.code, 2)
        self.assertEqual(self.station.dev.get_string("holding", ps.SITE_NAME), "NORTH-01")

    def test_client_survives_an_injected_exception(self):
        self.station.http("PUT", "/api/devices/1/faults/exception", {"exception_code": 6})
        with self.assertRaises(ModbusException) as raised:
            read_status(self.plc)
        self.assertEqual(raised.exception.code, 6)
        self.station.http("DELETE", "/api/devices/1/faults/exception")
        self.assertEqual(read_status(self.plc)["site"], "NORTH-01")


if __name__ == "__main__":
    unittest.main()
