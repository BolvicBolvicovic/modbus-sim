# `(tls (ca-certificate ...))`

Names the CA used to verify client certificates.

## Syntax

```scheme
(ca-certificate "PATH")
```

## Default

none

## Valid in

- `(tls ...)`

## Behaviour

1-255 characters. The CA is also advertised to clients as the acceptable client CA list. It is
required while [`require-client-certificate`](tls-require-client-certificate.md) is on (the default).

## Examples

```scheme
(tls (certificate "/etc/modbus/server.pem") (private-key "/etc/modbus/server.key")
     (ca-certificate "/etc/modbus/ca.pem"))
```

## See also

- [require-client-certificate](tls-require-client-certificate.md)
- [tls](tls.md)
