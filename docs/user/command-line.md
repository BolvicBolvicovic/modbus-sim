# Command line

```text
mogbus [options] CONFIG.scm
```

There are no server CLI options: the transport mode, path and all server settings are selected in `(server ...)` in the config file.

## Options

```text
  -c, --check          check the config, print a summary and exit
      --health         probe the running server described by the config; exit 0 if healthy, 1 if not
      --timeout-ms N   reply timeout of --health in milliseconds (default 2000)
  -v, --verbose        log every request
      --log-format F   log lines as text (default) or json
  -V, --version        print the version (and git revision, when built from a checkout) and exit
  -h, --help           show this help
```

## Exit status

| Status | Meaning |
|---|---|
| 0 | success, or a clean stop on SIGINT or SIGTERM |
| 1 | a config error, a serial or PTY open failure, or a runtime poll error |
| 2 | invalid CLI arguments, or a listener (TCP/UDP bind) or TLS start-up failure |

## Ports

Port 502 is the Modbus standard but needs root on Linux; the plaintext TCP default here is **1502**,
or **802** when `(tls ...)` is enabled.

## `--check`

`mogbus --check CONFIG.scm` prints the same summary as a normal start (transport, devices, point
counts, faults) and validates the server settings, including TLS files, without opening any socket or
device. When TLS is enabled it creates (and destroys) a TLS context, so missing or mismatched files and
bad ciphers fail with exit status 1 instead of printing `config OK`.

```bash
./mogbus --check profiles/power-meter.scm
```

## `--health`

`mogbus --health CONFIG.scm` is a liveness probe for containers and scripts: it loads the config like
`--check`, sends one Modbus FC 08 echo request to the first device over the configured network
transport (`tcp`, `udp`, `rtu-tcp`, `rtu-udp`, `ascii-tcp`, `ascii-udp`; a TLS server is only
connected to) and exits 0 if the unit answers, 1 otherwise with the reason on stderr. Serial and PTY
modes cannot be probed. See [Health check](deployment/health-check.md).

## See also

- [Logs](observability/logs.md)
- [Get started](get-started.md)
