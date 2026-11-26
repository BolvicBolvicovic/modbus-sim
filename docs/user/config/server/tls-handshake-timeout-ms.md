# `(tls (handshake-timeout-ms ...))`

Closes a TLS connection whose handshake is not complete in time.

## Syntax

```scheme
(handshake-timeout-ms N)
```

## Default

`10000`

## Valid in

- `(tls ...)`

## Behaviour

`N` is 1-86400000 milliseconds, measured from `accept()`. It is independent of
[`idle-timeout-ms`](idle-timeout-ms.md), which still defaults to `0`. A client that leaves the
handshake unfinished is closed at the deadline and logged at `WARN` with its peer address and the
configured timeout, freeing its slot for other clients. Connections that completed the handshake are
not affected; normal TLS requests and delay faults keep their existing timing.

## Examples

```scheme
(tls (certificate "/etc/modbus/server.pem") (private-key "/etc/modbus/server.key")
     (require-client-certificate off) (handshake-timeout-ms 5000))
```

## See also

- [idle-timeout-ms](idle-timeout-ms.md)
- [tls](tls.md)
