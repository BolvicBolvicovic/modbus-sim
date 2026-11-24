#ifndef TESTS_TLS_CLIENT_H
#define TESTS_TLS_CLIENT_H

#include "vlib/c_types.h"
#include <stdbool.h>

/* Minimal blocking OpenSSL client for the TLS integration tests. The opaque
 * SSL pointer keeps OpenSSL headers out of the network test suite. */

typedef struct ssl_st SSL;

typedef struct tls_client_t tls_client_t;
struct tls_client_t
{
	int	fd;
	SSL*	ssl;
};

/* Connects to 127.0.0.1:port and verifies the server against `ca` for IP
 * 127.0.0.1. Pass PEM cert/key to authenticate the client, or NULL/NULL for an
 * anonymous client. */
bool	tls_client_open(u16 port, const char* cert, const char* key, const char* ca,
			tls_client_t* out);
/* Same as tls_client_open but with a caller-chosen handshake/socket timeout. */
bool	tls_client_open_timeout(u16 port, const char* cert, const char* key, const char* ca,
				u32 timeout_ms, tls_client_t* out);
void	tls_client_close(tls_client_t* client);
bool	tls_client_write_all(tls_client_t* client, const u8* buf, u32 len);
/* Writes `len` bytes in a single SSL_write call, for tests that need one TLS record. */
bool	tls_client_write_record(tls_client_t* client, const u8* buf, u32 len);
bool	tls_client_read_exact(tls_client_t* client, u8* buf, u32 len);
const char* tls_client_error(void);

#endif
