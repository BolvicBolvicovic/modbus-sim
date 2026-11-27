/**
 * @file
 * @brief Transport-independent server facade over every transport.
 */
#ifndef SERVER_H
#define SERVER_H

#include "net/ascii_server.h"
#include "net/ascii_tcp_server.h"
#include "net/ascii_udp_server.h"
#include "net/net_obs.h"
#include "net/net_tls.h"
#include "net/rtu_server.h"
#include "net/rtu_tcp_server.h"
#include "net/rtu_udp_server.h"
#include "net/tcp_server.h"
#include "net/udp_server.h"
#include "server/server_limits.h"
#include "server/server_mode.h"
#include "os/os_serial.h"
#include "vlib/c_types.h"
#include <stdbool.h>

typedef struct device_t device_t;
typedef struct server_options_t server_options_t;
typedef struct server_t server_t;

/**
 * @brief What to run, independent of any config file.
 *
 * Strings and devices are borrowed and must outlive the server. Fields a mode does not use are
 * ignored (see the per-transport option structs in net).
 */
struct server_options_t
{
	const char*	id;			/**< names the server in logs and device assignments; NULL = none */
	server_mode_t	mode;
	const char*	bind_address;		/**< TCP-family and UDP modes */
	u16		port;			/**< TCP-family and UDP modes */
	const char*	path;			/**< *_DEVICE modes */
	int		fd;			/**< *_FD modes: borrowed, never closed */
	u32		seed;
	u32		idle_timeout_ms;	/**< TCP-family modes; 0 = disabled */
	u32		frame_gap_ms;		/**< RTU modes; 0 = auto */
	u32		baud;			/**< RTU timing and serial line speed; 0 = unknown */
	bool		strict_t15;		/**< RTU byte streams */
	bool		broadcast_execute;	/**< RTU modes */
	u32		data_bits;		/**< ASCII serial modes: 7 or 8 (RTU always uses 8) */
	parity_t	parity;			/**< serial modes */
	u32		stop_bits;		/**< serial modes */
	bool		tls_enabled;		/**< SERVER_MODE_TCP only */
	u32		tls_handshake_timeout_ms;
	net_tls_config_t tls;
	const char*	http_bind_address;	/**< HTTP health/metrics endpoint; NULL = off */
	u16		http_port;
	bool		http_control;		/**< serve the runtime control API under /api/ */
	bool		http_ui;		/**< serve the web page on `/`; implies http_control */
	const char*	http_allowed_hosts;	/**< extra Host names for /api/ and the page, comma-separated; NULL = none */
	device_t**	devices;
	u32		device_count;
};

/**
 * @brief One running transport plus the serial fds the facade opened itself.
 *
 * Caller-owned, stable address between server_init and server_close.
 */
struct server_t
{
	server_mode_t	mode;
	/** @brief The transport selected by `mode`; only that member is in use. */
	union
	{
		tcp_server_t		tcp;
		rtu_tcp_server_t	rtu_tcp;
		ascii_tcp_server_t	ascii_tcp;
		rtu_server_t		rtu;
		ascii_server_t		ascii;
		udp_server_t		udp;
		ascii_udp_server_t	ascii_udp;
		rtu_udp_server_t	rtu_udp;
	};
	net_obs_t	obs;		/**< metrics, request logs and the HTTP endpoint */
	char		id[SERVER_ID_MAX];	/**< log prefix; "" = none */
	bool		done;		/**< a group step saw it finish (stopped, or its byte stream ended) */
	int		fd;		/**< opened by init in PTY/device modes, else -1 */
	int		slave_fd;	/**< PTY slave kept open, else -1 */
	char		pty_name[128];
};

/**
 * @brief Opens the PTY/serial device when the mode needs one, then starts the transport.
 *
 * Returns 0, 1 if the serial fd could not be opened/configured, or 2 if a TCP/UDP listener or
 * TLS context could not be created. Nothing is left open on failure.
 */
s32	server_init(server_t* server, const server_options_t* options);

/** @brief One poll iteration (see NET_STEP_* in net/net_common.h). */
s32	server_step(server_t* server, s32 max_wait_ms);

/**
 * @brief One poll iteration of several servers with a single wait (see net_poll_group_t).
 *
 * @param servers Servers to step; NULL entries and servers that already finished are skipped.
 * @param count Number of entries in @p servers.
 * @param group Scratch for the shared wait, caller-owned.
 * @param max_wait_ms Longest wait; negative = until the nearest deadline of any server (at most 1 s).
 * @return NET_STEP_CONTINUE while some server is live, NET_STEP_DONE once every one has finished
 *         (or when there is none), NET_STEP_ERROR if a wait or a server failed.
 */
s32	server_group_step(server_t** servers, u32 count, net_poll_group_t* group, s32 max_wait_ms);

/** @brief Asks the server to stop; async-signal-safe, the next step returns NET_STEP_DONE. */
void	server_stop(server_t* server);

/**
 * @brief Closes the transport and any fds server_init opened.
 *
 * Idempotent after init.
 */
void	server_close(server_t* server);

/** @brief PTY slave path in PTY modes, else NULL. */
const char*	server_pty_name(const server_t* server);

/**
 * @brief Steps until stopped or the byte-stream peer goes away, then closes.
 *
 * Returns 0 after a clean stop, 1 on a poll error.
 */
s32	server_serve(server_t* server);

/** @brief server_init + server_serve; non-zero if it could not start. */
s32	server_run(server_t* server, const server_options_t* options);

/**
 * @brief True when TLS is off or a TLS context can be created from the options (creates and
 *        destroys one in `tls`); otherwise tls->error says why.
 */
bool	server_check_tls(const server_options_t* options, net_tls_ctx_t* tls);

#endif
