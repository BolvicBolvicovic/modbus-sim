/**
 * @file
 * @brief Config loader: the parsed (server ...) section and the device list.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include "build_config.h"
#include "vlib/c_types.h"
#include "vlib/arena.h"
#include "device/device.h"
#include "server/server_limits.h"
#include "server/server_mode.h"
#include "os/os_serial.h"
#include <stdbool.h>

typedef enum config_tls_version_t config_tls_version_t;
typedef struct config_t config_t;

#define CONFIG_DEFAULT_FRAME_GAP_MS	20	/**< fallback when baud is unknown and no explicit gap is set */
#define CONFIG_DEFAULT_TLS_PORT		802	/**< IANA mbap-s; plaintext TCP stays on 1502 */

/** @brief Minimum TLS version accepted by a TLS-enabled TCP server. */
enum config_tls_version_t
{
	CONFIG_TLS_VERSION_1_2,
	CONFIG_TLS_VERSION_1_3,
};

/**
 * @brief Everything read from a config file.
 *
 * Devices live in the arena given to config_load.
 */
struct config_t
{
	char		error[1024];		/**< why the last config_load() failed; "" otherwise */
	server_mode_t	mode;
	char		bind_address[SERVER_BIND_MAX];
	char		path[SERVER_PATH_MAX];
	u16		port;
	u32		seed;			/**< seed for fault dice; 0 = fixed default */
	u32		idle_timeout_ms;	/**< close idle connections after N ms; 0 = disabled */
	u32		frame_gap_ms;		/**< RTU inter-frame silence override in ms; 0 = auto (baud-derived or default) */
	u32		baud;			/**< RTU serial baud rate; 0 = unknown */
	bool		strict_t15;		/**< RTU byte streams: opt-in t1.5 discard heuristic */
	bool		broadcast_execute;	/**< RTU transports: execute unit-0 requests on all devices */
	u32		data_bits;		/**< ASCII byte streams: 7 or 8 */
	parity_t	parity;			/**< RTU and ASCII byte streams: serial parity */
	u32		stop_bits;		/**< RTU and ASCII byte streams: 1 or 2 */
	bool		http_enabled;		/**< (http ...): health, readiness and metrics endpoint */
	u16		http_port;
	bool		http_control;		/**< (http (control on)): runtime control API under /api/ */
	bool		http_ui;		/**< (http (ui on)): web page on `/` (implies control) */
	char		http_allowed_hosts[256];	/**< (http (allowed-host "NAME")): comma-separated, at most 8 */
	char		http_bind_address[SERVER_BIND_MAX];
	bool		tls_enabled;		/**< tcp mode only: wrap MBAP in TLS */
	u32		tls_handshake_timeout_ms;	/**< TLS only: deadline from accept to handshake completion */
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

/** @brief Maps a parity name to its enum value. */
bool		config_parity_from_name(const char* name, parity_t* out);

/**
 * @brief Reads a Scheme config file.
 *
 * - persistent: arena that keeps the devices (must outlive the config)
 * - scratch   : arena for temporary data; may be cleared afterwards
 * On failure returns false and leaves a message a human can act on in out->error.
 */
bool	config_load(arena_t* persistent, arena_t* scratch, const char* path, config_t* out);

#endif
