#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include "net/net_tls.h"
#include "vlib/c_types.h"

typedef struct device_t device_t;

/* Name: tcp_server_options_t
 * Description: what the server needs to run. Devices are borrowed, not copied.
 * */
typedef struct tcp_server_options_t tcp_server_options_t;
struct tcp_server_options_t
{
	const char*	bind_address;
	u16		port;
	u32		seed;
	u32		idle_timeout_ms;	/* close idle connections after N ms; 0 = disabled */
	bool		tls_enabled;		/* wrap MBAP in TLS; borrowed settings from config_t */
	const char*	tls_certificate;
	const char*	tls_private_key;
	const char*	tls_ca_certificate;
	bool		tls_require_client_certificate;
	net_tls_version_t tls_min_version;
	const char*	tls1_2_ciphers;
	const char*	tls1_3_ciphersuites;
	device_t**	devices;
	u32		device_count;
};

/* Name: tcp_server_run
 * Description: serves Modbus TCP until SIGINT or SIGTERM. Single thread, no blocking sleeps:
 * a delayed response never stalls other clients.
 * Returns 0 after a clean stop, non-zero if the server could not start.
 * */
s32	tcp_server_run(const tcp_server_options_t* options);

#endif
