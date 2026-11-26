# `(tls (private-key ...))`

Names the server private key.

## Syntax

```scheme
(private-key "PATH")
```

## Default

none (required)

## Valid in

- `(tls ...)`

## Behaviour

PEM format, 1-255 characters. Its contents are never logged.

## See also

- [certificate](tls-certificate.md)
- [tls](tls.md)
