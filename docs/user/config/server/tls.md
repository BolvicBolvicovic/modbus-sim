# `(server (tls ...))`

Wraps Modbus TCP in TLS: the same MBAP header and PDU, inside a TLS connection.

## Syntax

```scheme
(tls (certificate "PATH") (private-key "PATH")
     [(ca-certificate "PATH")] [(handshake-timeout-ms N)]
     [(require-client-certificate on|off)] [(min-version tls1.2|tls1.3)]
     [(tls1-2-ciphers "LIST")] [(tls1-3-ciphersuites "LIST")])
```

## Default

absent (plaintext); the default port becomes `802` when present

## Valid in

- `tcp`

## Behaviour

No Modbus framing or function-code behavior changes. `(tls ...)` is rejected at load time, including
by `--check`, in every other mode (even as an empty `(tls)` form). A binary built with `WITH_TLS=0`
has no OpenSSL support: `(tls ...)` is rejected at load time in every mode. Plaintext TCP is
unchanged when `(tls ...)` is absent, in either build.

Keys: [certificate](tls-certificate.md), [private-key](tls-private-key.md),
[ca-certificate](tls-ca-certificate.md), [handshake-timeout-ms](tls-handshake-timeout-ms.md),
[require-client-certificate](tls-require-client-certificate.md), [min-version](tls-min-version.md),
[tls1-2-ciphers](tls-ciphers.md) and [tls1-3-ciphersuites](tls-ciphers.md).

Paths are checked for non-emptiness during config load. When TLS is enabled, both `--check` and
server startup create the OpenSSL context: a missing or unreadable certificate/key/CA, a CA whose name
list cannot be loaded, a mismatched certificate and private key, or an invalid cipher list makes
`--check` fail with exit status 1 and no `config OK` line with `--check=verbose` (startup fails with the same error). TLS
1.0, TLS 1.1, SSLv3 and anonymous/NULL ciphers are always disabled, and user cipher strings cannot
lower the configured minimum. The configured CA is advertised to clients as the acceptable client CA
list, but revocation checking (CRL/OCSP) and Modbus Security role authorization from x.509 role OIDs
are not implemented: a trusted client certificate is either required or optional for every device.

A client certificate's subject is logged at `INFO` once the handshake succeeds. When the server closes
a TLS connection (idle timeout, I/O error, EOF, or shutdown), it makes one nonblocking best-effort
attempt to send `close_notify` before freeing the session, so clients can observe a clean TLS close
without delaying other clients. An ungraceful peer drop reported with an empty OpenSSL error queue is
treated as EOF and is not logged as a TLS failure.

## Errors

- `built without TLS support`: the binary was built with `WITH_TLS=0`; remove `(tls ...)` or use a TLS build.

## Examples

```scheme
(server (mode tcp) (bind "127.0.0.1")
  (tls (certificate "/etc/modbus/server.pem")
       (private-key "/etc/modbus/server.key")
       (ca-certificate "/etc/modbus/ca.pem")
       (handshake-timeout-ms 5000)))
```

## See also

- [certificate](tls-certificate.md)
- [require-client-certificate](tls-require-client-certificate.md)
- [Modbus support](../../modbus/modbus-overview.md)
