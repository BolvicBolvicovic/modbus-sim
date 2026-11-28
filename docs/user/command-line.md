# Command line

```text
mogbus [options] CONFIG.scm
mogbus --decode [--framing F] [--direction D] [HEX...]
```

There are no server CLI options: the transport mode, path and all server settings are selected in `(server ...)` in the config file.

## Options

```text
  -c, --check[=verbose]
                       check the config and exit; prints nothing when it is valid, =verbose prints the summary
      --health         probe the running server described by the config; exit 0 if healthy, 1 if not
      --timeout-ms N   reply timeout of --health in milliseconds (default 2000)
  -w, --web            serve the web UI on the HTTP endpoint (turns the control API on too)
  -r, --show-registers[=MS]
                       show the point values every MS ms (10-60000, default 1000): a live table on a terminal
      --decode         decode captured frames instead of running a server (see below)
      --framing F      with --decode: auto (default), mbap, rtu or ascii
      --direction D    with --decode: auto (default), request or response
  -v, --verbose        log every request
      --log-format F   log lines as text (default) or json
  -V, --version        print the version (and git revision, when built from a checkout) and exit
  -h, --help           show this help
```

## `--web`

`mogbus --web CONFIG.scm` (or `-w`) serves the [web UI](observability/web-ui.md) and the control API
it uses. With no `(http ...)` section in the config it binds `127.0.0.1` port `9102`; with one it
keeps its `bind` and `port` and turns the UI on. The summary printed at start-up (and by `--check=verbose`)
shows the address as `web UI on http://127.0.0.1:9102/`; for a wildcard bind (`0.0.0.0` or `::`) it shows the
bind address and where the UI is reachable on this machine, for example
`web UI on http://0.0.0.0:9102/ (reachable at http://127.0.0.1:9102/)`. There is no authentication: see the
[security notes](observability/web-ui.md#security).

## `--show-registers`

`mogbus --show-registers CONFIG.scm` (or `-r`; `--show-registers=250` sets the interval in
milliseconds, 10 to 60000, default 1000) shows the current value of every configured point while the
server runs, in every mode including serial and PTY. It is the `(server (show-registers MS))` option of
the library (see [show-registers](config/server/show-registers.md)); the flag sets it on every server
of the file.

On a terminal one table (unit, table, address, type, value) is redrawn in place, with the refresh
interval and the achieved refresh rate in its first line; log lines are kept below it instead of
scrolling it away, and written to stderr once when the server stops (the pane is erased first). When stdout is not a terminal (a pipe
or a file) the output is line oriented, as below. The first print is a full snapshot; after that only the
points whose value changed since the last print appear, so a busy simulator stays readable. Values are
decoded as configured (type, word and byte order): integers and floats as numbers, coils and discrete
inputs as `0` or `1`, string points as quoted text. Points have no names, so a line is identified by
unit, table and first address. Reads go through the device like a host read, so ramps, models and
clocks show their value at the time of the print and nothing changes on the wire. Log lines stay on
stderr.

```text
unit 1 holding 0 = 450
unit 1 input 0 = 200
unit 1 input 1 = 0
unit 1 input 2 = 0
unit 1 input 1 = 1
unit 1 input 2 = 104
unit 1 holding 0 = 600
```

With `--log-format json` each line is a JSON object instead:

```text
{"unit":1,"space":"input","address":1,"value":1}
{"unit":1,"space":"holding","address":0,"value":600}
```

An interval outside 10-60000, or one that is not a number, exits with status 2. The interval is
the time between checks, not a guarantee: a transport busy answering a request delays the next
print a little.

## `--decode`

`mogbus --decode HEX...` decodes captured frames and prints every field of the ADU (MBAP header, RTU
or ASCII address and checksum) and of the PDU; it reads no config and opens nothing. A frame is hex,
with or without spaces, `:`, `,` or `0x` between the bytes, or the ASCII frame text starting with `:`
(CR LF is added when missing). Quote a frame that has spaces, since each argument is one frame. With
no argument it reads one frame per line from stdin (blank lines and lines starting with `#` are
skipped). `--framing` and `--direction` force what `auto` would detect; with `auto` the output says
why it chose, and a frame that is valid in several framings lists every candidate.
`--log-format json` prints one JSON object per frame instead of text.

```bash
./mogbus --decode "00 01 00 00 00 06 01 03 00 00 00 02"
./mogbus --decode --log-format json ":010300000001FB"
./mogbus --decode --framing rtu --direction response "01 03 02 00 0A 38 43"
tshark -r capture.pcap -Y modbus -T fields -e tcp.payload | ./mogbus --decode
```

The exit status is 0 when every frame decoded cleanly, 1 when one is malformed (wrong checksum or
length, truncated: what could be decoded is still printed, then the problems), and 2 for text that
is not hex or an invalid option. The output, the fields and the rules for detecting the framing and
the direction are in [Decoding frames](modbus/decoding-frames.md). The same decoder is
@ref mogbus_decode_frame "mogbus_decode_frame()" in C and `mogbus.decode_frame()` in Python.

## Exit status

| Status | Meaning |
|---|---|
| 0 | success, or a clean stop on SIGINT or SIGTERM |
| 1 | a config error, a serial or PTY open failure, or a runtime poll error; with `--decode`, a malformed frame |
| 2 | invalid CLI arguments (with `--decode`, also text that is not hex), or a listener (TCP/UDP bind) or TLS start-up failure |

## Ports

Port 502 is the Modbus standard but needs root on Linux; the plaintext TCP default here is **1502**,
or **802** when `(tls ...)` is enabled.

## `--check`

`mogbus --check CONFIG.scm` validates the config and the server settings, including TLS files, without
opening any socket or device, and exits. It prints nothing and exits 0 when everything is valid; on a
problem it prints the error on stderr and exits 1. `mogbus --check=verbose CONFIG.scm` also prints the
same summary as a normal start (transport, devices, point counts, faults) and `config OK`. When TLS is
enabled it creates (and destroys) a TLS context, so missing or mismatched files and bad ciphers fail
with exit status 1 instead of printing `config OK`.

```bash
./mogbus --check profiles/power-meter.scm
./mogbus --check=verbose profiles/power-meter.scm
```

## `--health`

`mogbus --health CONFIG.scm` is a liveness probe for containers and scripts: it loads the config like
`--check`, sends one Modbus FC 08 echo request to the first device over the configured network
transport (`tcp`, `udp`, `rtu-tcp`, `rtu-udp`, `ascii-tcp`, `ascii-udp`; a TLS server is only
connected to) and exits 0 if the unit answers, 1 otherwise with the reason on stderr. Serial and PTY
modes cannot be probed. See [Health check](deployment/health-check.md).

## See also

- [Logs](observability/logs.md)
- [Decoding frames](modbus/decoding-frames.md)
- [Get started](get-started.md)
