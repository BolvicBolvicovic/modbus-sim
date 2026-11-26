# Logs

Logs go to stderr, as plain text or as one JSON object per line.

`-v` / `--verbose` adds one `DEBUG` line per request and per TCP connect and disconnect.
`--log-format json` writes every line as a JSON object instead of text:

```bash
./mogbus -v --log-format json my-device.scm
```

```json
{"ts":"2026-11-26T09:42:52.164Z","level":"debug","msg":"tcp 127.0.0.1:38042 unit=2 fc=0x03 -> exception 0x02","event":"request","transport":"tcp","peer":"127.0.0.1:38042","unit":2,"fc":3,"result":"exception","exception":2,"latency_ms":0,"faults":[]}
```

Every line has `ts` (UTC, millisecond precision), `level` (`debug`, `info`, `warn`) and `msg` (the
text line).

## Request lines

Request lines (`"event":"request"`) add:

| Field | Meaning |
|---|---|
| `transport` | the server mode, e.g. `tcp`, `rtu-udp` |
| `peer` | client `ip:port`; absent on serial and PTY byte streams |
| `unit` | unit id from the frame (0 = broadcast) |
| `fc` | function code |
| `result` | `ok`, `exception`, `none` or `dropped`, as in `mogbus_responses_total` |
| `exception` | exception code, 0 when the result is not `exception` |
| `latency_ms` | scheduled response delay from a `delay` fault |
| `faults` | fault kinds that fired for this request, e.g. `["delay","corrupt"]` |

## Connection lines

Connection lines have `"event":"connect"` or `"event":"disconnect"` with `transport` and `peer`.

## From a program

A program that embeds the library selects the format with `mogbus_set_log_format()`; a log sink then
receives the whole JSON object as its message. See [Lifetimes and threading](../library/library-lifetimes.md).

## See also

- [Metrics](metrics/metrics.md)
- [Command line](../command-line.md)
