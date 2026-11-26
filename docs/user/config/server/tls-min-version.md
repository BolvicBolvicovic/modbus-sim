# `(tls (min-version ...))`

Sets the minimum TLS version.

## Syntax

```scheme
(min-version tls1.2|tls1.3)
```

## Default

`tls1.2`

## Valid in

- `(tls ...)`

## Behaviour

TLS 1.0, TLS 1.1 and SSLv3 are always disabled, and cipher strings cannot lower this minimum.

## See also

- [tls1-2-ciphers](tls-ciphers.md)
- [tls](tls.md)
