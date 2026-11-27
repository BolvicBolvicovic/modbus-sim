#!/usr/bin/env python3
"""A simulated water pumping station: the complete example of the Python tutorial.

The station is one Modbus device (unit 1) built with the mogbus Python API, served over TCP from a
background thread, and driven from the host by a small process model: the tank level follows the
pump, the alarms follow the level. A tiny stdlib client (modbus_tcp.py) plays the part of "your
SCADA / data logger / control code". Exit status 0 means every check passed.

From a checkout:  make python && PYTHONPATH=python python3 examples/python/pump_station.py
Walk-through:     docs/user/python/python-start.md
"""
import contextlib
import json
import struct
import sys
import threading
import time
import urllib.request
from pathlib import Path

import mogbus
from modbus_tcp import Client, ModbusException

ROOT = Path(__file__).resolve().parents[2]

# Register map (the same on every tutorial of the documentation).
SETPOINT, MODE, SITE_NAME = 0, 2, 10          # holding: float32, u16, string of 8 registers
RUN_COMMAND = 0                               # coil
HIGH_ALARM, LOW_ALARM, RUNNING = 0, 1, 2      # discrete inputs
LEVEL, OUTFLOW, CURRENT, INFLOW, RUN_SECONDS = 0, 2, 4, 6, 10   # input registers
EVENT_FILE, ALARM_FIFO = 1, 100               # file records and FIFO queue

OFF, AUTO, MANUAL = 0, 1, 2
FC_STATUS_BLOCK = 0x41                        # vendor function code: read the station status block
(EV_HIGH_ON, EV_HIGH_OFF, EV_LOW_ON, EV_LOW_OFF, EV_PUMP_ON, EV_PUMP_OFF, EV_SETPOINT) = range(1, 8)
EVENT_RECORDS = 10                            # the event log keeps the last 10 events
SETPOINT_RANGE = (0.5, 4.5)                   # metres; anything else is refused
TANK_HEIGHT, TANK_AREA = 5.0, 2.0             # m, m2
PUMP_FLOW = 90.0                              # m3/h when the pump runs
HIGH_LEVEL, LOW_LEVEL = 4.0, 0.5              # alarm thresholds, m


def build_station(ctx):
    """Adds the pumping station (unit 1) to a context and returns it."""
    dev = ctx.add_device(1, "Pump station")

    dev.add_point("holding", SETPOINT, "f32", 2.5)                      # level setpoint, m
    dev.add_point("holding", MODE, "u16", AUTO)
    dev.add_point("holding", SITE_NAME, "string", length=8, text="NORTH-01", read_only=True)
    dev.add_point("coil", RUN_COMMAND, "bool", 0)                       # pump run command (manual mode)
    for address in (HIGH_ALARM, LOW_ALARM, RUNNING):
        dev.add_point("discrete", address, "bool", 0)
    dev.add_point("input", LEVEL, "f32", 2.0)                           # tank level, m
    dev.add_point("input", OUTFLOW, "f32", 0.0)                         # m3/h
    dev.add_point("input", CURRENT, "u16", 0)                           # motor current, 0.1 A
    dev.add_point("input", RUN_SECONDS, "u32", 0)                       # pump run seconds
    # The inlet flow moves by itself like a real one, and a legacy meter sends the low word first.
    dev.add_point("input", INFLOW, "f32", word_order="low-first",
                  ramp={"from": 20, "to": 60, "step": 4, "every_ms": 1000, "mode": "bounce"})

    # An event log (file 1, one register per record) and an alarm history (FIFO queue).
    dev.add_file_record(EVENT_FILE, 0, [0] * EVENT_RECORDS)
    dev.add_fifo(ALARM_FIFO, [])
    return dev


class Process:
    """The physics of the tank, run from the host.

    tick(dt) advances the model by dt simulated seconds: the pump follows the mode (auto holds the
    level around the setpoint with a 0.25 m band, manual follows the run command, off stops it),
    the level follows the flows, and the alarms, the feedback and the counters follow the level.
    It reads what clients wrote (setpoint, mode, run command) and writes what the plant would
    measure with Device.set_value(), which makes those points host-owned.
    """

    def __init__(self, dev):
        self.dev = dev
        self.pump = False
        self.seconds = 0.0          # simulated pump run time
        self.high = self.low = False
        self.alarms = []
        self.thread = None
        self._stop = threading.Event()

    def event(self, code):
        """Appends to the event log (file 1) and, for alarms, to the alarm FIFO (newest last)."""
        log = self.dev.read_file_records(EVENT_FILE, 0, EVENT_RECORDS)
        self.dev.write_file_records(EVENT_FILE, 0, log[1:] + [code])
        if code in (EV_HIGH_ON, EV_LOW_ON):
            self.alarms = (self.alarms + [code])[-31:]
            self.dev.set_fifo(ALARM_FIFO, self.alarms)

    def tick(self, dt):
        dev = self.dev
        setpoint = dev.get_value("holding", SETPOINT)
        mode = int(dev.get_value("holding", MODE))
        level = dev.get_value("input", LEVEL)
        was_running = self.pump
        if mode == AUTO:
            if level >= setpoint + 0.25:
                self.pump = True
            elif level <= setpoint - 0.25:
                self.pump = False
        elif mode == MANUAL:
            self.pump = bool(dev.get_value("coil", RUN_COMMAND))
        else:
            self.pump = False
        outflow = PUMP_FLOW if self.pump else 0.0
        inflow = dev.get_value("input", INFLOW)
        level = min(max(level + (inflow - outflow) / 3600.0 * dt / TANK_AREA, 0.0), TANK_HEIGHT)
        if self.pump:
            self.seconds += dt
        dev.set_value("input", LEVEL, level)
        dev.set_value("input", OUTFLOW, outflow)
        dev.set_value("input", CURRENT, round(10 * (10 + 2 * level)) if self.pump else 0)
        dev.set_value("input", RUN_SECONDS, int(self.seconds))
        dev.set_value("discrete", RUNNING, self.pump)
        if self.pump != was_running:
            self.event(EV_PUMP_ON if self.pump else EV_PUMP_OFF)
        high, low = level >= HIGH_LEVEL, level <= LOW_LEVEL
        for now, before, on, off, address in ((high, self.high, EV_HIGH_ON, EV_HIGH_OFF, HIGH_ALARM),
                                              (low, self.low, EV_LOW_ON, EV_LOW_OFF, LOW_ALARM)):
            if now != before:
                dev.set_value("discrete", address, now)
                self.event(on if now else off)
        self.high, self.low = high, low

    def start(self, speed=1.0, period=0.05):
        """Ticks on a thread, `speed` simulated seconds per real second."""
        def loop():
            last = time.monotonic()
            while not self._stop.wait(period):
                now = time.monotonic()
                self.tick((now - last) * speed)
                last = now
        self._stop.clear()
        self.thread = threading.Thread(target=loop, name="process", daemon=True)
        self.thread.start()

    def stop(self):
        self._stop.set()
        if self.thread is not None:
            self.thread.join()
            self.thread = None


class Policy:
    """The access hook: an audit trail of client writes, and the rules that refuse bad ones."""

    def __init__(self, dev, process=None):
        self.dev = dev
        self.process = process
        self.audit = []             # (function code, space, address, values) of accepted writes
        self.refused = 0

    def __call__(self, access):
        if access.kind != mogbus.AccessKind.WRITE or access.target != mogbus.AccessTarget.TABLE:
            return None
        values = access.values
        first, last = access.address, access.address + access.count - 1
        if access.space == mogbus.Space.HOLDING:
            if first <= SETPOINT + 1 and last >= SETPOINT:
                # The setpoint is a float32 in two registers: both must be written together.
                if first > SETPOINT or last < SETPOINT + 1:
                    return self.refuse(access)
                i = SETPOINT - first
                setpoint = struct.unpack(">f", struct.pack(">HH", values[i], values[i + 1]))[0]
                if not SETPOINT_RANGE[0] <= setpoint <= SETPOINT_RANGE[1]:
                    return self.refuse(access)
            if first <= MODE <= last and values[MODE - first] > MANUAL:
                return self.refuse(access)
        self.audit.append((access.function_code, access.space.name.lower(), access.address, list(values)))
        return None

    def refuse(self, access):
        self.refused += 1
        return 3                    # illegal data value

    def written(self, access):
        """The post-write hook: runs only once the write was applied, so a refused setpoint never logs."""
        if (access.target == mogbus.AccessTarget.TABLE and
                access.space == mogbus.Space.HOLDING and
                access.address <= SETPOINT < access.address + access.count and
                self.process is not None):
            self.process.event(EV_SETPOINT)


def add_status_block(dev, process, policy):
    """Defines the vendor function code 0x41, "read station status block", computed at request time.

    Request data: one byte, the block id (1). Reply data: block id, mode, flags (bit 0 high alarm,
    1 low alarm, 2 pump running), level in cm (u16), outflow in 0.1 m3/h (u16), refused writes (u16).
    An unknown block id is answered with exception 03.
    """
    def status(request):
        if request != b"\x01":
            return 3
        flags = process.high | (process.low << 1) | (process.pump << 2)
        return struct.pack(">BBBHHH", 1, int(dev.get_value("holding", MODE)), flags,
                           round(dev.get_value("input", LEVEL) * 100),
                           round(dev.get_value("input", OUTFLOW) * 10), policy.refused)

    dev.add_function_code(FC_STATUS_BLOCK, status)


class Station:
    """A station served on loopback, for tests: see running_station()."""

    def __init__(self, ctx, dev, process, policy, server, options):
        self.ctx, self.dev, self.process, self.policy, self.server = ctx, dev, process, policy, server
        self.port = options.port
        self.http_port = options.http_port

    def client(self):
        return Client(self.port)

    def http(self, method, path, body=None):
        request = urllib.request.Request(f"http://127.0.0.1:{self.http_port}{path}", method=method,
                                         data=None if body is None else json.dumps(body).encode())
        with urllib.request.urlopen(request, timeout=2) as reply:
            return json.loads(reply.read() or b"null")


@contextlib.contextmanager
def running_station():
    """Builds, hooks and serves a station on free loopback ports; closes everything on exit.

    The process model is not ticking: a test calls station.process.tick(dt) to move simulated time.
    """
    with mogbus.Context() as ctx:
        dev = build_station(ctx)
        process = Process(dev)
        policy = Policy(dev, process)
        dev.set_access_hook(policy)
        dev.set_written_hook(policy.written)
        add_status_block(dev, process, policy)
        dev.set_value("input", INFLOW, 40.0)        # no ramp: the same level curve on every run
        options = mogbus.ServerOptions(port=free_port(), http_bind_address="127.0.0.1",
                                       http_port=free_port(), http_control=True)
        with ctx.serve(options).start() as server:
            yield Station(ctx, dev, process, policy, server, options)


def free_port():
    import socket
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for(condition, seconds=3.0):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline and not condition():
        time.sleep(0.02)


def heading(text):
    print(f"\n== {text} ==")


def check(what, ok):
    print(f"{'ok  ' if ok else 'FAIL'} {what}")
    if not ok:
        check.failed += 1


check.failed = 0


def main():
    print("mogbus", mogbus.library_version())

    # Step 1: build the device.
    heading("1. The device")
    ctx = mogbus.Context()
    dev = build_station(ctx)
    info = dev.info()
    print(f"{info.name!r}, unit {info.unit_id}, points per table: {info.point_counts}")
    print("site name from the host:", dev.get_string("holding", SITE_NAME))
    try:
        dev.add_point("holding", 1, "u16", 0)      # overlaps the setpoint's second register
    except mogbus.MogbusError as exc:
        print("rejected:", exc)

    # Step 2: serve it and talk to it like a client would.
    heading("2. Serve and read")
    process = Process(dev)
    policy = Policy(dev, process)
    dev.set_access_hook(policy)
    dev.set_written_hook(policy.written)
    add_status_block(dev, process, policy)
    options = mogbus.ServerOptions(port=free_port(), http_bind_address="127.0.0.1", http_port=free_port(),
                                   http_control=True)
    ctx.check_server(options)
    server = ctx.serve(options).start()
    print("alive:", mogbus.probe(options, 1, 1000))
    with Client(options.port) as plc:
        print("site:", plc.read_string(SITE_NAME, 8))
        print("setpoint:", plc.read_f32(3, SETPOINT), "mode:", plc.holding(MODE)[0])
        print("level registers:", plc.input(LEVEL, 2), "-> level", plc.read_f32(4, LEVEL))
        check("the client reads the site name", plc.read_string(SITE_NAME, 8) == "NORTH-01")
        try:
            plc.write_registers(SITE_NAME, [0x4F4B])
        except ModbusException as exc:
            print("write to the read-only name:", exc)
            check("the site name is read-only (exception 02)", exc.code == 2)

        # The ramp moves on its own; set_value pins it so the demo is repeatable.
        first = plc.read_f32(4, INFLOW, low_first=True)
        time.sleep(1.2)
        second = plc.read_f32(4, INFLOW, low_first=True)
        print(f"inlet flow moved by itself: {first:.0f} -> {second:.0f} m3/h")
        check("the ramp moves", first != second)
        dev.set_value("input", INFLOW, 40.0)
        time.sleep(1.1)
        check("set_value pins the ramp at 40", plc.read_f32(4, INFLOW, low_first=True) == 40.0)

        # Step 3: the process.
        heading("3. The process")
        print("minute  level  pump  run-s")
        for minute in range(0, 8):
            if minute:
                for _ in range(6):
                    process.tick(10.0)
            print(f"{minute:6d}  {plc.read_f32(4, LEVEL):5.2f}  {plc.discrete(RUNNING)[0]:4d}  "
                  f"{plc.input(RUN_SECONDS, 2)[1]:5d}")
        check("auto mode started the pump", plc.discrete(RUNNING)[0] == 1 or process.seconds > 0)

        # Step 4: a client changes the plant; the access hook has its say.
        heading("4. Writes, rules and the audit trail")
        try:
            plc.write_f32(SETPOINT, 9.0)
        except ModbusException as exc:
            print("setpoint 9.0 m:", exc)
            check("an out-of-range setpoint is refused with exception 03", exc.code == 3)
        try:
            plc.write_register(SETPOINT + 1, 0)
        except ModbusException as exc:
            check("half a float32 is refused too", exc.code == 3)
        plc.write_f32(SETPOINT, 3.0)
        print("setpoint now:", plc.read_f32(3, SETPOINT))
        plc.write_register(MODE, MANUAL)
        plc.write_coil(RUN_COMMAND, False)
        print("audit:", policy.audit)
        check("the setpoint changed", dev.get_value("holding", SETPOINT) == 3.0)
        check("two writes refused", policy.refused == 2)

        # Alarms: in manual mode with the pump stopped the tank fills up.
        heading("5. Alarms, the event log and the alarm FIFO")
        while not process.high:
            process.tick(10.0)
        print("high-level alarm:", plc.discrete(HIGH_ALARM)[0], "level", round(plc.read_f32(4, LEVEL), 2))
        plc.write_coil(RUN_COMMAND, True)
        while not process.low:
            process.tick(10.0)
        print("low-level alarm:", plc.discrete(LOW_ALARM)[0], "level", round(plc.read_f32(4, LEVEL), 2))
        check("both alarms raised", plc.discrete(LOW_ALARM)[0] == 1 and process.alarms == [EV_HIGH_ON, EV_LOW_ON])
        print("event log (file 1):", plc.file_records(EVENT_FILE, 0, EVENT_RECORDS))
        print("alarm FIFO:", plc.fifo(ALARM_FIFO))
        check("the FIFO holds both alarms, oldest first", plc.fifo(ALARM_FIFO) == [EV_HIGH_ON, EV_LOW_ON])

        # Vendor function code: the status block is computed when the request arrives.
        heading("5b. A vendor function code")
        block = plc.call(struct.pack(">BB", FC_STATUS_BLOCK, 1))
        block_id, mode, flags, level_cm, outflow, refused = struct.unpack(">BBBHHH", block[1:])
        print(f"status block: mode {mode}, flags {flags:03b}, level {level_cm / 100} m, "
              f"outflow {outflow / 10} m3/h, {refused} refused writes")
        check("the status block follows the live values",
              block_id == 1 and refused == 2 and flags & 2 and level_cm == round(plc.read_f32(4, LEVEL) * 100))
        try:
            plc.call(struct.pack(">BB", FC_STATUS_BLOCK, 9))
        except ModbusException as exc:
            check("an unknown block id is answered with exception 03", exc.code == 3)

        # Step 6: the process on a thread, while the server answers clients.
        heading("6. The process on a thread")
        plc.write_registers(MODE, [AUTO])
        process.start(speed=600.0)          # ten simulated minutes per second
        wait_for(lambda: plc.discrete(RUNNING)[0] == 1)
        print("auto mode: pump running =", bool(plc.discrete(RUNNING)[0]))
        check("the thread ticked while clients were served", plc.discrete(RUNNING)[0] == 1)
        plc.write_registers(MODE, [OFF])
        wait_for(lambda: plc.discrete(RUNNING)[0] == 0)
        process.stop()
        print("mode off: pump running =", bool(plc.discrete(RUNNING)[0]))
        check("mode off stops the pump", plc.discrete(RUNNING)[0] == 0)

        # Step 7: logs, metrics and the control API.
        heading("7. Logs, metrics, runtime control")
        lines = []
        mogbus.set_log_level("debug")
        mogbus.set_log_format("json")
        mogbus.set_log_sink(lambda level, message: lines.append((level.name, message)))
        plc.holding(SETPOINT, 2)
        mogbus.set_log_sink(None)
        mogbus.set_log_format("text")
        mogbus.set_log_level("warn")
        request = [json.loads(m) for _, m in lines if '"event":"request"' in m]
        keys = ("event", "fc", "result", "transport", "unit")
        print("a request logged as JSON:", json.dumps({k: request[-1][k] for k in keys}, sort_keys=True))
        check("the log sink saw the request", bool(lines))
        for line in server.metrics().splitlines():
            if line.split(" ")[0] in ('mogbus_requests_total{fc="3"}', 'mogbus_requests_total{fc="5"}'):
                print(line)
        base = f"http://127.0.0.1:{options.http_port}"
        urllib.request.urlopen(urllib.request.Request(
            base + "/api/devices/1/faults/exception", data=b'{"exception_code": 6}', method="PUT"), timeout=2)
        try:
            plc.holding(SETPOINT, 2)
        except ModbusException as exc:
            print("with the fault injected:", exc)
            check("the injected fault answers exception 06", exc.code == 6)
        urllib.request.urlopen(urllib.request.Request(
            base + "/api/devices/1/faults/exception", method="DELETE"), timeout=2)
        check("the fault is cleared", plc.holding(MODE) == [OFF])

    server.close()
    ctx.close()

    # Step 8: a profile instead of code.
    heading("8. A profile from a file")
    with mogbus.serve(config_text='(device (unit-id 1) (holding 0 (static 7)))', port=0) as srv:
        with Client(srv.port) as plc:
            print("config_text:", plc.holding(0))
            check("an inline config serves its value", plc.holding(0) == [7])
    with mogbus.serve(config=str(ROOT / "profiles" / "flow-meter.scm"), port=0) as srv:
        srv.device(1).set_value("input", 0, 12.5)
        with Client(srv.port) as meter:
            print("flow meter from profiles/flow-meter.scm:", meter.read_f32(4, 0), "m3/h")
            check("the profile serves the value set by the host", meter.read_f32(4, 0) == 12.5)

    # Step 9: the options of a config, other transports.
    heading("9. Options from a config, other transports")
    with mogbus.Context() as ctx2:
        ctx2.load_config(str(ROOT / "profiles" / "flow-meter.scm"))
        wanted = ctx2.config_server_options()
        print(f"the profile asks for {wanted.mode.name.lower()} on port {wanted.port}")
        for mode in ("tcp", "udp", "rtu-tcp", "ascii-tcp"):
            ctx2.check_server(wanted, mode=mode, port=free_port())
        rtu = mogbus.ServerOptions(mode="rtu-tcp", port=free_port())
        with ctx2.serve(rtu).start():
            alive = mogbus.probe(rtu, 1, 1000)
            print("rtu-tcp server probe:", alive)
            check("the same device answers over RTU-over-TCP", alive)
        print("RTU timing at 19200 baud:", mogbus.rtu_timing(19200))

    print("\nall checks passed" if not check.failed else f"\n{check.failed} check(s) failed")
    return 1 if check.failed else 0


if __name__ == "__main__":
    sys.exit(main())
