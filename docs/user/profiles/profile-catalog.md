# Profile catalog

The ready-made profiles, what each device is and what it shows. Each profile starts with a comment listing its register map.

| Profile | Unit | Device and what it shows | What it offers |
|---|---|---|---|
| [`minimal.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/minimal.scm) | 1 | Two registers: the smallest config | holding 0 fixed, holding 1 counts 0-100 |
| [`modbus-udp.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/modbus-udp.scm) | 1 | Two registers over UDP: the `(mode udp)` transport | same registers served over Modbus UDP (`mode udp`), with a small delay fault |
| [`ascii-udp.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/ascii-udp.scm) | 1 | Two registers, ASCII over UDP: the `(mode ascii-udp)` transport | same registers served as ASCII frames over UDP (`mode ascii-udp`), with a small delay fault |
| [`rtu-udp.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/rtu-udp.scm) | 1 | Two registers over RTU-over-UDP: the `(mode rtu-udp)` transport | same registers served as RTU frames over UDP (`mode rtu-udp`), with a small delay fault |
| [`power-meter.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/power-meter.scm) | 1 | Three-phase power meter: float32 values, ramps, energy counter | 3 voltages, 3 currents, power, frequency, power factor, energy (float32 input registers); 2 settings; reset coil; alarm |
| [`energy-meter.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/energy-meter.scm) | 1 | Energy meter: u64 energy counter, float64, byte-swapped float32, string registers | u64 energy counter (Wh), float64 power, byte-swapped (BADC) float32 voltage and current, string serial number and model name; CT ratio; reset coil |
| [`temperature-controller.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/temperature-controller.scm) | 1 | Temperature controller: signed values, setpoint, alarms | process value (s16, 0.1 C), output %, setpoint and mode (writable), run coil, 2 alarms |
| [`heater.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/heater.scm) | 1 | Heater with a thermostat: setpoint-following lag, gaussian noise, sine ambient | temperature that lags a writable setpoint with gaussian noise, ambient temperature (sine), setpoint |
| [`aging-sensor.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/aging-sensor.scm) | 1 | Pressure transmitter that wears out: drift, growing noise, stuck value, then exception 04 | pressure that drifts and gets noisier after 1 h, sticks at 3 h and fails with exception 04 at 6 h; a healthy reference pressure |
| [`vfd-drive.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/vfd-drive.scm) | 1 | Motor drive: start-up ramp that holds, 32-bit counter | status, frequency and current that climb and hold, DC voltage, temperature, 32-bit energy; control word, setpoint, run and reset coils |
| [`motor.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/motor.scm) | 1 | Motor with a scripted machine: start/stop states, timed ramp, random trip | scripted state machine: idle/starting/running/tripped, timed ramp and random trip |
| [`flow-meter.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/flow-meter.scm) | 1 | Flow meter: float32 and 32-bit totalizer | flow rate and temperature (float32), 32-bit totalizer, reset coil, empty-pipe alarm |
| [`io-module.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/io-module.scm) | 1 | Remote I/O block: `(count N)` for many points | 16 digital inputs, 16 digital outputs, 8 analog inputs (4-20 mA), 4 analog outputs |
| [`slow-sensor.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/slow-sensor.scm) | 1 | Sensor with late answers: delay fault | temperature sensor; 30% of answers arrive 1.5 s late |
| [`unreliable-meter.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/unreliable-meter.scm) | 1 | Meter that goes silent: drop fault | energy counter; 20% of answers never arrive |
| [`two-devices.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/two-devices.scm) | 1, 2 | Two devices, one port: unit ids | two devices on one port |
| [`http-endpoint.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/http-endpoint.scm) | 1 | Process rig with the HTTP endpoint: health, metrics, control API and web UI on 127.0.0.1:9102 | writable setpoint, input that lags it with noise, bouncing ramp, sine wave; open the web UI and plot them |
| [`vendor-function-codes.scm`](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/vendor-function-codes.scm) | 1 | Vendor-style device: user-defined function codes 0x41 and 0x64 | status, serial number, setpoint, 2 live inputs; user-defined function codes 0x41 (fixed reply, live registers, echo) and 0x64 (acknowledge, exception, no response) |

## See also

- [Using a profile](using-profiles.md)
- [Write your own profile](write-your-own-profile.md)
- [Config reference](../config/format.md)
