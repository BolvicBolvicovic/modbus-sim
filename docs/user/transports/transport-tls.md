# Modbus TCP with TLS (`tcp` + `tls`)

`(server (tls ...))` wraps the MBAP exchange of `mode tcp` in TLS, the transport-security part of Modbus/TCP Security.

## Syntax

```scheme
(server (mode tcp)
  (tls (certificate "server.pem")
       (private-key "server.key")
       (ca-certificate "ca.pem")
       (require-client-certificate on)
       (handshake-timeout-ms 10000)))
```

## Default

port 802, client certificates required, TLS 1.2 minimum, `handshake-timeout-ms 10000`

## Valid in

- `tcp` mode only
- builds with TLS support

## Behaviour

- The bytes inside the tunnel are the same MBAP frames and get the same unit routing, fault handling
  and PDU answers as plaintext TCP.
- A server certificate and private key are required. Client certificates are required by default,
  and then the CA certificate is required too; `(require-client-certificate off)` simulates anonymous
  clients, and the CA entry becomes optional. This is a testing relaxation, not a recommended setting.
- TLS 1.2 is the minimum and anonymous and NULL ciphers are disabled. TLS defaults to port 802 when
  `(port N)` is omitted, so publish that port or set `(port 1502)`.
- `--check` validates the TLS files and ciphers by creating the context, just as startup does.
- An accepted connection must finish its handshake within `(tls (handshake-timeout-ms N))`
  (1 to 86400000, default `10000`), measured from accept and independent of `idle-timeout-ms`.
  Stalled handshakes are closed and logged at `WARN` with the peer and the timeout, so they cannot
  hold all 32 client slots. Connections that finished the handshake are unaffected.
- A client certificate's subject is logged at `INFO` after a successful handshake, so the
  authenticated peer can be audited. Private keys and secrets are never logged.
- A configured CA is advertised to clients as the acceptable client CA list; a failure to load that
  list fails context creation.
- Every TLS close (idle timeout, I/O error, EOF or server shutdown) makes one nonblocking
  best-effort attempt to send `close_notify` and never waits for the peer. An ungraceful peer drop
  reported with an empty OpenSSL error queue is treated as EOF, not logged as a TLS failure.
- TLS is an optional build feature: `WITH_TLS=auto` enables it when OpenSSL is installed and
  `WITH_TLS=0` omits it. Plaintext TCP, UDP, RTU and ASCII transports work the same in both builds.
- `(tls ...)` is rejected at load time in every other mode, and in every mode of a no-TLS build with
  `built without TLS support`.

### Modbus/TCP Security coverage

TLS mode implements the transport-security part of the Modbus/TCP Security specification: the MBAP
exchange is wrapped in TLS 1.2 or 1.3, the server presents a certificate, mutual TLS can require a
client certificate signed by the configured CA, the CA is advertised in the TLS certificate request,
TLS versions and cipher suites are configurable, and the authenticated peer subject is logged.

It is not full Modbus Security role-based access control. Certificate revocation (CRL/OCSP) is not
checked, Modbus Security x.509 role OIDs are not interpreted, and a trusted client certificate grants
access to every configured device and unit. Clients must trust the server certificate out of band;
the advertised CA does not provision client trust stores.

## Errors

| Message | Meaning |
|---|---|
| `built without TLS support` | the binary was built with `WITH_TLS=0` |
| `--check` fails with exit status 1 | missing or mismatched certificate files, or a bad cipher |

## Examples

Example PEM files from `tests/tls`; replace them with your own paths:

```scheme
(server (mode tcp) (bind "127.0.0.1")
  (tls (certificate "tests/tls/server.pem")
       (private-key "tests/tls/server.key")
       (ca-certificate "tests/tls/ca.pem")))

(device (name "TLS device") (unit-id 1) (holding 0 (static 42)))
```

## See also

- [Modbus TCP](transport-tcp.md)
- [Config reference: TLS](../config/server/tls.md)
- [Not supported (yet)](../modbus/modbus-overview.md#not-supported-yet)
