#ifndef CONFIG_H
#define CONFIG_H

#include "build_config.h"
#include "vlib/c_types.h"
#include "vlib/arena.h"
#include "device/device.h"
#include "server/server_limits.h"
#include "server/server_mode.h"
#include "util/serial.h"
#include <stdbool.h>

#define CONFIG_DEFAULT_FRAME_GAP_MS	20	/* fallback when baud is unknown and no explicit gap is set */
#define CONFIG_DEFAULT_TLS_PORT		802	/* IANA mbap-s; plaintext TCP stays on 1502 */

/* Name: config_tls_version_t
 * Description: minimum TLS version accepted by a TLS-enabled TCP server.
 * */
typedef enum config_tls_version_t config_tls_version_t;
enum config_tls_version_t
{
	CONFIG_TLS_VERSION_1_2,
	CONFIG_TLS_VERSION_1_3,
};

/* Name: config_t
 * Description: everything read from a config file.
 * Devices live in the arena given to config_load.
 * */
typedef struct config_t config_t;
struct config_t
{
	server_mode_t	mode;
	char		bind_address[SERVER_BIND_MAX];
	char		path[SERVER_PATH_MAX];
	u16		port;
	u32		seed;			/* seed for fault dice; 0 = fixed default */
	u32		idle_timeout_ms;	/* close idle connections after N ms; 0 = disabled */
	u32		frame_gap_ms;		/* RTU inter-frame silence override in ms; 0 = auto (baud-derived or default) */
	u32		baud;			/* RTU serial baud rate; 0 = unknown */
	bool		strict_t15;		/* RTU byte streams: opt-in t1.5 discard heuristic */
	bool		broadcast_execute;	/* RTU transports: execute unit-0 requests on all devices */
	u32		data_bits;		/* ASCII byte streams: 7 or 8 */
	parity_t	parity;			/* RTU and ASCII byte streams: serial parity */
	u32		stop_bits;		/* RTU and ASCII byte streams: 1 or 2 */
	bool		tls_enabled;		/* tcp mode only: wrap MBAP in TLS */
	u32		tls_handshake_timeout_ms;	/* TLS only: deadline from accept to handshake completion */
	char		tls_certificate[SERVER_TLS_PATH_MAX];
	char		tls_private_key[SERVER_TLS_PATH_MAX];
	char		tls_ca_certificate[SERVER_TLS_PATH_MAX];
	bool		tls_require_client_certificate;
	config_tls_version_t tls_min_version;
	char		tls1_2_ciphers[SERVER_TLS_CIPHER_MAX];
	char		tls1_3_ciphersuites[SERVER_TLS_CIPHER_MAX];
	u32		device_count;
	device_t*	devices[SERVER_MAX_DEVICES];
};

/* Maps a parity name to its enum value. */
bool		config_parity_from_name(const char* name, parity_t* out);

/* Name: config_load
 * Description: reads a Scheme config file.
 * - persistent: arena that keeps the devices (must outlive the config)
 * - scratch   : arena for temporary data; may be cleared afterwards
 * On failure returns false and fills `err` with a message a human can act on.
 * */
bool	config_load(arena_t* persistent, arena_t* scratch, const char* path,
		    config_t* out, char* err, u32 err_size);

#endif
