# `(server (id ...))`

Names a server so devices can say which servers serve them.

## Syntax

```scheme
(id "NAME")
```

## Default

none. With a single `(server ...)` the id is optional and the server is the default one; with
several, every server needs its own.

## Valid in

- all modes

## Behaviour

A file may hold up to 8 `(server ...)` forms. Each runs its own transport and all of them run
together in one `mogbus` process, so one config can serve the same devices over TCP, RTU-over-TCP
and UDP at once, or different devices on different ports. `NAME` is 1-31 letters, digits, `.`, `-`
or `_` and must be unique in the file. The id prefixes the server's log lines (`[plant] listening
on ...`), the `--check` summary and `--health --server NAME`. Devices pick their servers with
[servers](../device/servers.md). Two servers cannot listen on the same address, port and
protocol (TCP and UDP ports are separate), or open the same serial device; `--check` reports the
clash.

## Errors

- `server: id 'X' must be 1 to 31 letters, digits, '.', '-' or '_'`: use a shorter name made of those characters.
- `server N has no (id "NAME"); every server needs one when the file has several`: add `(id ...)` to every server.
- `servers N and M both use id 'X'`: give each server its own id.
- `too many (server ...) forms (max 8)`: keep at most 8 servers.
- `servers 'A' and 'B' both listen on TCP port N (...)`: give them different ports or bind addresses.
- `servers 'A' and 'B' both use the serial device PATH`: one serial line, one server.

## Examples

```scheme
(server (id "plant")  (mode tcp)     (port 1502))
(server (id "legacy") (mode udp)     (port 1502))
(device (unit-id 1) (servers "plant" "legacy") (holding 0 (static 1)))
```

## See also

- [servers](../device/servers.md)
- [server](server.md)
- [Profile multi-server.scm](https://github.com/BolvicBolvicovic/mogbus/blob/main/profiles/multi-server.scm)
