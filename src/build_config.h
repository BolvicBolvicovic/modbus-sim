#ifndef BUILD_CONFIG_H
#define BUILD_CONFIG_H

/* The Makefile passes -DWITH_TLS=1/0 to every project source and test. This
 * fallback keeps direct/manual compilations on the no-OpenSSL path. */
#ifndef WITH_TLS
#define WITH_TLS 0
#endif

#endif
