# `(tls (require-client-certificate ...))`

Requires clients to present a certificate signed by the configured CA.

## Syntax

```scheme
(require-client-certificate on|off)
```

## Default

`on`

## Valid in

- `(tls ...)`

## Behaviour

[`ca-certificate`](tls-ca-certificate.md) is required while this is on. Set it to `off` and omit the
CA to simulate anonymous clients; this is a testing relaxation, not a recommended production setting.

## Examples

```scheme
(tls (certificate "/etc/modbus/server.pem") (private-key "/etc/modbus/server.key")
     (require-client-certificate off))
```

## See also

- [ca-certificate](tls-ca-certificate.md)
- [tls](tls.md)
