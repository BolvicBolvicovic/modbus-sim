# `(server (show-registers ...))`

Shows the point values of every device while the server runs: a live table on a terminal, lines otherwise.

## Syntax

```scheme
(show-registers MS)
```

## Default

Off. `MS` is the refresh interval in milliseconds, 10 to 60000.

## Valid in

- all modes

## Behaviour

The view belongs to the whole process, not to one server: when any server of the file sets it, every
device of the file is shown, at the smallest interval asked for. The command-line option
`--show-registers[=MS]` (`-r`) sets it on every server and so wins over the config.

- **On a terminal** (stdout is a terminal, `TERM` is not `dumb`, the log format is not JSON) one
  table is redrawn in place. The first line shows the refresh interval, the rate achieved between the
  last two redraws, the local time and the number of devices and points; then come the columns unit,
  table, address, type and value, one line per point. The table never exceeds the terminal height: when
  it does not fit the last line says `... N more points`. Log lines would corrupt the drawing, so
  while the table is on they are kept (the last six) and shown in a pane below it, and written to
  stderr once when the server stops (the pane is erased first). The summary and PTY paths printed before the start stay above the
  table. On Windows the console must support virtual-terminal sequences (Windows 10 or newer).
- **Otherwise** (a pipe, a file, or `--log-format json`) the view prints a full snapshot first and then
  only the points whose value changed, one line per point, as `unit U table A = VALUE` or as a JSON
  object with `--log-format json`. Log lines stay on stderr.

Values are decoded as configured: integers and floats as numbers, coils and discrete inputs as `0` or
`1`, string points as quoted text. Reads go through the device like a host read, so ramps, models and
clocks show their value at the time of the redraw and nothing changes on the wire. The view is drawn
by `mogbus_step()` and `mogbus_run()` (and so by a Python `Server` that steps); `mogbus_server_step()`
alone does not draw it.

## Errors

- `(show-registers 9): must be between 10 and 60000`: use a number of milliseconds in that range.
- `server: option 'show-registers' is given more than once`: write it once per server.

## Examples

```scheme
(server (mode tcp) (port 1502) (show-registers 250))
(device (unit-id 1) (holding 0 (ramp (from 0) (to 100) (step 1) (every-ms 100))))
```

## See also

- [Command line](../../command-line.md)
- [Logs](../../observability/logs.md)
- [http-ui](http-ui.md)
