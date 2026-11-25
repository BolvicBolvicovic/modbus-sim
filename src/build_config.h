#ifndef BUILD_CONFIG_H
#define BUILD_CONFIG_H

/* The Makefile passes -DWITH_TLS=1/0 to every project source and test. This
 * fallback keeps direct/manual compilations on the no-OpenSSL path. */
#ifndef WITH_TLS
#define WITH_TLS 0
#endif

/* WITH_CONFIG=0 leaves out the Scheme config loader (src/config, s7). The
 * Makefile always passes it; direct compilations get the default. */
#ifndef WITH_CONFIG
#define WITH_CONFIG 1
#endif

#endif
