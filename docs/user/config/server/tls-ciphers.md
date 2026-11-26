# `(tls (tls1-2-ciphers ...))` and `(tls (tls1-3-ciphersuites ...))`

Set the OpenSSL cipher list for TLS 1.2 and below, and the cipher suite list for TLS 1.3.

## Syntax

```scheme
(tls1-2-ciphers "LIST")
(tls1-3-ciphersuites "LIST")
```

## Default

the OpenSSL default

## Valid in

- `(tls ...)`

## Behaviour

Each list must be non-empty and at most 511 characters. Anonymous and NULL ciphers stay disabled, and
an invalid list makes `--check` and startup fail.

## See also

- [min-version](tls-min-version.md)
- [tls](tls.md)
