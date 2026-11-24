#ifndef CONFIG_H
#define CONFIG_H

#include "vlib/c_types.h"
#include "vlib/arena.h"
#include "device/device.h"
#include "util/serial.h"
#include <stdbool.h>

#define CONFIG_MAX_DEVICES		16
#define CONFIG_DEFAULT_PORT		1502
#define CONFIG_DEFAULT_IDLE_TIMEOUT_MS	0
#define CONFIG_DEFAULT_FRAME_GAP_MS	20	/* fallback when baud is unknown and no explicit gap is set */
#define CONFIG_DEFAULT_BAUD		0	/* 0 = unknown; derive RTU timing from tty when possible */
#define CONFIG_BIND_MAX			128
#define CONFIG_PATH_MAX			256
#define CONFIG_TLS_PATH_MAX		256
#define CONFIG_TLS_CIPHER_MAX		512
#define CONFIG_DEFAULT_TLS_PORT		802	/* IANA mbap-s; plaintext TCP stays on 1502 */

/* Name: server_mode_t
 * Description: transport selected by (server (mode NAME)).
 * */
typedef enum server_mode_t server_mode_t;
enum server_mode_t
{
	SERVER_MODE_TCP,
	SERVER_MODE_RTU_PTY,
	SERVER_MODE_RTU_DEVICE,
	SERVER_MODE_RTU_TCP,
	SERVER_MODE_ASCII_PTY,
	SERVER_MODE_ASCII_DEVICE,
	SERVER_MODE_ASCII_TCP,
};

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
	char		bind_address[CONFIG_BIND_MAX];
	char		path[CONFIG_PATH_MAX];
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
	char		tls_certificate[CONFIG_TLS_PATH_MAX];
	char		tls_private_key[CONFIG_TLS_PATH_MAX];
	char		tls_ca_certificate[CONFIG_TLS_PATH_MAX];
	bool		tls_require_client_certificate;
	config_tls_version_t tls_min_version;
	char		tls1_2_ciphers[CONFIG_TLS_CIPHER_MAX];
	char		tls1_3_ciphersuites[CONFIG_TLS_CIPHER_MAX];
	u32		device_count;
	device_t*	devices[CONFIG_MAX_DEVICES];
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
