# Embedding mogbus in C

The `mogbus` binary is one front end. The same code builds as a static and a shared library with a small C API, so a test harness or application can embed simulated Modbus devices and drive them from its own loop.

The only header is [`include/mogbus.h`](https://github.com/BolvicBolvicovic/mogbus/blob/main/include/mogbus.h).
It needs just `<stdint.h>`, `<stdbool.h>` and `<stddef.h>`, and every symbol is `mogbus_*` or
`MOGBUS_*`. The current version is 0.0.1 (`MOGBUS_VERSION_*`, `mogbus_version()`). The full list of
functions is in the C API reference of this site.

From Python, see [Using mogbus from Python](../python/python-start.md): a `ctypes` package over
`libmogbus.so`.

## Build and link

```bash
make -j$(nproc) lib          # libmogbus.a + libmogbus.so
make -j$(nproc) lib-check    # also builds and runs examples/embed_tcp.c against both
```

`make` alone builds only the `mogbus` binary, which is itself a client of this API linked against
the static library. `make WITH_CONFIG=0` builds the libraries instead, since there is no binary then.
The `.a` contains the whole simulator, including the vendored s7 parser, so it is self-contained.
The `.so` exports only `mogbus_*` symbols; everything else is hidden.

```bash
# shared (found at run time via rpath or LD_LIBRARY_PATH)
cc -Iinclude app.c -L. -lmogbus -Wl,-rpath,'$ORIGIN' -o app
# static: also link what the library needs (-lm, plus -lssl -lcrypto in a TLS build)
cc -Iinclude app.c libmogbus.a -lm -lssl -lcrypto -o app
```

A `WITH_TLS=0` build has no OpenSSL dependency; creating a server with `tls_enabled` then fails with
an error. Objects are compiled with `-fPIC` (valid inside the PIE binary too), so one object set
serves the binary and both libraries.

## Build options

- `WITH_TLS=auto|0|1` (default `auto`): OpenSSL support, see [Build from source](../building.md).
- `WITH_CONFIG=1|0` (default `1`): `0` leaves out the config file loader (`src/config`,
  `src/server/server_config.c`) and the vendored s7, so the libraries hold no s7 code and serve only
  devices built in code. The header and the exported symbols are identical, so the ABI does not
  change: `mogbus_load_config()` returns false and `mogbus_server_create_from_config()` returns
  NULL, both with the error `built without config support`. A `WITH_CONFIG=0` build has no `mogbus`
  binary (it needs a config), so `make` builds `libmogbus.a` and `.so`. Combine freely with `WITH_TLS=0`.

## A first program

[`examples/embed_tcp.c`](https://github.com/BolvicBolvicovic/mogbus/blob/main/examples/embed_tcp.c)
includes only `mogbus.h`. It creates a context, adds a device with one holding register, starts a TCP
server on a free loopback port, connects as a raw MBAP client in the same process, calls
`mogbus_server_step()` between client I/O, and checks the value it reads back. `make lib-check`
builds it against `libmogbus.so` (rpath `$ORIGIN/..`) and `libmogbus.a` and runs both.

```c
mogbus_t* ctx = mogbus_create();
mogbus_device_t* dev = mogbus_add_device(ctx, 1, "embedded", err, sizeof(err));
mogbus_point_t point = { .space = MOGBUS_HOLDING, .address = 10, .type = MOGBUS_TYPE_U16, .value = 1234 };
mogbus_device_add_point(dev, &point, err, sizeof(err));

mogbus_server_options_t options = mogbus_server_options_defaults();
options.port = 1502;
mogbus_server_t* server = mogbus_server_create(ctx, &options, err, sizeof(err), NULL);
while (mogbus_server_step(server, 10) == MOGBUS_STEP_CONTINUE)
    /* host work here */;
mogbus_server_close(server);
mogbus_destroy(ctx);
```

## See also

- [C API overview](library-api.md)
- [Data types and points](library-data.md)
- [Lifetimes and threading](library-lifetimes.md)
