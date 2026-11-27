<p align="center">

![mogbus](docs/assets/mogbus-lockup.svg)

</p>

# mogbus

A Modbus device simulator. You describe a device in a small text file; `mogbus` pretends to be that
device on the network (TCP or UDP) or on a serial byte stream. Point your SCADA, PLC, gateway or test
script at it.

```scheme
(server (mode tcp) (port 1502))

(device
  (name "Minimal example")
  (unit-id 1)
  (holding 0 (static 42))                                                      ; never changes
  (holding 1 (ramp (from 0) (to 100) (step 10) (every-ms 1000) (mode wrap))))  ; counts up
```

A config file is a short list of `(name value)` entries. The syntax is inspired by Scheme, but it is
plain data: there is no code to write, only settings to fill in. Start from one of the ready-made
profiles and change what you need.

## Features

- **Ten transports:** Modbus TCP (optionally TLS), UDP, and RTU and ASCII over a serial line, a PTY,
  TCP or UDP.
- **19 function codes**, including diagnostics, file records, FIFO queues, device identification and
  your own vendor codes.
- **Realistic data:** 16, 32 and 64-bit integers, floats, strings, any word and byte order; values that
  ramp, follow a setpoint, drift, wear out or are driven by a scripted state machine.
- **Faults on demand:** delays, drops, exceptions, corrupted frames, disconnects and restarts, by
  chance or by schedule.
- **Observable:** health and readiness endpoints, Prometheus metrics, JSON logs, and a runtime
  control API.
- **Embeddable:** a small C library and a Python package for tests and scripts.
- **Easy to deploy:** one binary, a hardened Docker image and Kubernetes manifests.

See [What mogbus does](docs/user/features.md) for the full table.

## Quick start

```bash
git clone --recurse-submodules https://github.com/BolvicBolvicovic/mogbus.git
cd mogbus
make -j$(nproc)                        # builds ./mogbus
./mogbus profiles/minimal.scm          # start the simulator on port 1502

# in another terminal:
./tools/modbus_probe.py holding 0 2    # -> [42, <a value that moves>]
```

`./mogbus --check CONFIG.scm` validates a config without starting the server. The
[Get started](docs/user/get-started.md) page walks through writing your own config in five minutes.

## Where to go next

| I want to | Read |
|---|---|
| start from a ready-made device | [Using a profile](docs/user/profiles/using-profiles.md), [Profile catalog](docs/user/profiles/profile-catalog.md) |
| convert a vendor register map | [Importing a register map](docs/user/import/import-overview.md) |
| look up a config entry | [Config reference](docs/user/config/format.md) |
| know what the server answers | [Modbus support](docs/user/modbus/modbus-overview.md), [Function codes](docs/user/modbus/function-codes.md), [Exception codes](docs/user/modbus/exceptions.md) |
| pick a transport | [Transports](docs/user/transports/transports.md) |
| run it in a container | [Run mogbus in Docker](docs/user/deployment/docker.md), [Kubernetes](docs/user/deployment/kubernetes.md) |
| monitor or control it | [Health, readiness and metrics](docs/user/observability/http-endpoint.md), [Runtime control API](docs/user/observability/api/control-api.md), [Logs](docs/user/observability/logs.md) |
| use it from code | [Embedding mogbus in C](docs/user/library/library-start.md), [Using mogbus from Python](docs/user/python/python-start.md) |
| use it on Windows | [Running mogbus on Windows](docs/user/windows/windows-route.md) |
| build with other options | [Build from source](docs/user/building.md), [Command line](docs/user/command-line.md) |
| work on mogbus itself | [CONTRIBUTING.md](CONTRIBUTING.md), [Architecture](docs/dev/architecture.md), [Release process](docs/dev/release.md) |

## Safe by default

TCP and UDP listeners bind to loopback (`127.0.0.1`) by default, so they accept only local clients.
Bind explicitly to `0.0.0.0` (or `::` for IPv6) to accept remote connections; see
[Transports](docs/user/transports/transports.md#bind-addresses). The plaintext TCP default port is
**1502**, since port 502 needs root on Linux.

## Contributing and security

- [CONTRIBUTING.md](CONTRIBUTING.md) - how to propose changes
- [SECURITY.md](SECURITY.md) - how to report security issues

## License

This project is licensed under the [MIT License](https://github.com/BolvicBolvicovic/mogbus/blob/main/LICENSE). Third-party components are documented in
[NOTICE.md](NOTICE.md).
