# `(device (servers ...))`

Lists the servers that serve the device.

## Syntax

```scheme
(servers "ID" ["ID" ...])
```

## Default

With a single `(server ...)` (named or not) every device belongs to it. With several, the key is
required.

## Valid in

- `(device ...)`

## Behaviour

Each `ID` is the [id](../server/id.md) of a `(server ...)` of the file. A device may list up to 8
servers and then answers on all of them with the same registers, faults and machines. A server
only answers the units of the devices that list it: another server's unit gets the unknown-unit
answer of that transport. Unit ids have to be unique per server, not per file, so two devices can
both be unit 1 when they belong to different servers. At most one device of the file can set
[any-unit](any-unit.md).

Each server needs at least one device. A device is simulated once, even when several servers
reach it.

## Errors

- `device 'A' is not assigned to a server; add (servers "ID" ...)`: the file has several servers, so every device must list its servers.
- `device 'A': (servers ...) names 'X', but no (server (id "X")) exists`: fix the name or add the server.
- `device 'A': servers is given more than once`: keep one `(servers ...)` entry.
- `device 'A': server 'X' is listed twice`: list each server once.
- `servers: expected (servers "ID" ...) with at least one server id`: add at least one id.
- `server 'X' serves no device; add (servers "X") to a device`: assign a device or remove the server.
- `devices 'A' and 'B' both use unit-id N`: two devices of the same server need different unit ids.

## Examples

```scheme
(server (id "tcp") (mode tcp) (port 1502))
(server (id "rtu") (mode rtu-tcp) (port 1503))
(device (unit-id 1) (servers "tcp" "rtu") (holding 0 (static 1)))
(device (unit-id 2) (servers "rtu") (holding 0 (static 2)))
```

Unit 1 answers on both ports; unit 2 only on 1503.

## See also

- [id](../server/id.md)
- [unit-id](unit-id.md)
- [server-id](server-id.md): the unrelated FC 17 Report Server ID byte
