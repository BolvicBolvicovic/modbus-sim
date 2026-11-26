# `(tls (certificate ...))`

Names the server certificate chain.

## Syntax

```scheme
(certificate "PATH")
```

## Default

none (required)

## Valid in

- `(tls ...)`

## Behaviour

PEM format, 1-255 characters.

## Examples

```scheme
(tls (certificate "/etc/modbus/server.pem") (private-key "/etc/modbus/server.key")
     (require-client-certificate off))
```

## See also

- [private-key](tls-private-key.md)
- [tls](tls.md)
