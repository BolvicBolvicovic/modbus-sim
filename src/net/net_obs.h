/**
 * @file
 * @brief Observability of one server: request metrics, structured request logs, and the
 *        HTTP health, readiness and Prometheus endpoint.
 *
 * Everything runs on the server's own thread: transports record into net_obs_t and poll the
 * endpoint's sockets together with their own through net_poll(). Nothing is allocated.
 */
#ifndef NET_OBS_H
#define NET_OBS_H

#include "modbus/pdu.h" // IWYU pragma: keep
#include "os/os_mutex.h"
#include "os/os_socket.h"
#include "os/os_wait.h"
#include "vlib/c_types.h"
#include <stdbool.h>

typedef enum net_result_t net_result_t;
typedef enum net_frame_dir_t net_frame_dir_t;
typedef enum net_close_reason_t net_close_reason_t;
typedef struct net_obs_t net_obs_t;
typedef enum net_poll_phase_t net_poll_phase_t;
typedef struct net_poll_group_t net_poll_group_t;

/** @brief Limits of the metrics and HTTP endpoint. */
enum
{
	NET_OBS_MAX_DEVICES	= 16,	/**< same as SERVER_MAX_DEVICES (checked in server.c) */
	NET_OBS_LATENCY_BUCKETS	= 12,
	NET_OBS_HTTP_CONNS	= 4,	/**< concurrent HTTP clients; others wait in the backlog */
	NET_OBS_HTTP_IN		= 1024,	/**< request head limit */
	NET_OBS_HTTP_BODY	= 1024,	/**< request body limit (runtime control) */
	NET_OBS_HTTP_OUT	= 32768,/**< response limit, enough for 16 fully used devices */
	NET_OBS_HOST_MAX	= 128,	/**< longest bind address kept for the Host check */
	NET_OBS_HOSTS_MAX	= 256,	/**< allowed Host names, comma-separated, with the NUL */
	NET_OBS_HOSTS_COUNT	= 8,	/**< most allowed Host names */
	NET_OBS_HTTP_TIMEOUT_MS	= 2000,	/**< a connection must be done within this */
	NET_OBS_POLL_MAX	= 64,	/**< transport fds plus endpoint fds per wait */
	NET_OBS_GROUP_MAX	= 512,	/**< most entries one shared group wait takes (8 servers of 38 entries each fit) */
};

/** @brief Phase of a net_poll_group_t. */
enum net_poll_phase_t
{
	NET_POLL_GATHER,	/**< net_poll() only records the wait table and returns -1 */
	NET_POLL_REPLAY,	/**< net_poll() takes the results of the shared wait instead of waiting */
};

/**
 * @brief Lets several servers share one os_wait().
 *
 * A group step runs every server's step twice: in the gather phase each net_poll() appends its
 * entries here and returns -1 without waiting (so the step returns before doing any I/O), then one
 * os_wait() covers all entries, then in the replay phase each step runs again and net_poll()
 * hands it the revents of its own entries by fd. Steps do only time-based, idempotent work
 * (idle sweeps, due delayed responses) before polling, so running them twice is harmless.
 */
struct net_poll_group_t
{
	net_poll_phase_t phase;
	os_mutex_t*	lock;		/**< released around the shared wait; NULL = none */
	int		timeout_ms;	/**< shortest timeout a gathered step asked for; -1 = none */
	u32		count;
	os_wait_entry_t	entries[NET_OBS_GROUP_MAX];
	const net_obs_t* owner[NET_OBS_GROUP_MAX];	/**< server that appended each entry */
};

/** @brief What happened to one request. */
enum net_result_t
{
	NET_RESULT_OK,		/**< normal response */
	NET_RESULT_EXCEPTION,	/**< Modbus exception response (from the device or a fault) */
	NET_RESULT_NONE,	/**< no response by protocol: broadcast, unknown serial unit, listen-only */
	NET_RESULT_DROPPED,	/**< response suppressed by a drop or disconnect fault */
	NET_RESULT_COUNT,
};

/** @brief Direction of a frame shown to the frame hook. */
enum net_frame_dir_t
{
	NET_FRAME_RECEIVED,
	NET_FRAME_SENT,
};

/**
 * @brief Frame hook: sees one transport frame and may rewrite it in place.
 *
 * `data` holds `len` bytes and has room for `capacity`. Returns the new length (0 drops the
 * frame; above `capacity` it is dropped with a warning). `peer` is NULL on a byte stream.
 */
typedef u32 (*net_frame_fn)(void* user, net_frame_dir_t dir, const char* peer, u8* data, u32 len,
			    u32 capacity);

/** @brief Why a client connection was closed, as shown to the connection hook. */
enum net_close_reason_t
{
	NET_CLOSE_NONE,		/**< the connection was opened */
	NET_CLOSE_PEER,		/**< the client closed it (EOF or hang-up) */
	NET_CLOSE_IDLE_TIMEOUT,	/**< no traffic for the idle timeout */
	NET_CLOSE_DISCONNECT_FAULT,	/**< a disconnect fault fired */
	NET_CLOSE_TLS,		/**< TLS setup, handshake failure or handshake timeout */
	NET_CLOSE_SERVER_STOP,	/**< the server was closed with the client connected */
	NET_CLOSE_ERROR,	/**< socket error or protocol violation (bad header, buffer full) */
};

/**
 * @brief Connection hook: a client connection opened (`reason` NET_CLOSE_NONE) or closed.
 *
 * `id` is unique per server and never reused; `peer` is the log text of the client.
 */
typedef void (*net_conn_fn)(void* user, u64 id, const char* peer, net_close_reason_t reason);

/** @brief Upper bounds of the response latency histogram, in milliseconds. */
extern const u32 NET_OBS_LATENCY_MS[NET_OBS_LATENCY_BUCKETS];

/**
 * @brief Observability state of one server.
 *
 * Zero-initialized means "record nothing, serve nothing" until net_obs_init().
 */
struct net_obs_t
{
	const char*	transport;	/**< mode name for logs and metrics, e.g. "tcp" */
	device_t**	devices;	/**< borrowed; metrics are kept per index */
	u32		device_count;
	u64		start_unix_s;	/**< wall-clock start, for mogbus_start_time_seconds */
	_Atomic bool	ready;		/**< serving; false before start and once stopping (stop may come from another thread) */
	os_mutex_t*	lock;		/**< context lock, released around the wait in net_poll(); NULL = none */
	net_poll_group_t* group;	/**< shared wait while a group step runs, else NULL */
	net_frame_fn	frame_fn;	/**< frame hook, or NULL; set by the host after net_obs_init() */
	void*		frame_user;
	net_conn_fn	conn_fn;	/**< connection hook, or NULL; set by the host after net_obs_init() */
	void*		conn_user;

	/** @brief Counters exposed on `/metrics`. */
	struct net_metrics_t
	{
		u64	requests_by_fc[256];
		u64	responses[NET_RESULT_COUNT];
		u64	frames_rejected;	/**< malformed, bad checksum or oversized frames */
		u64	latency_bucket[NET_OBS_LATENCY_BUCKETS];	/**< non-cumulative counts */
		u64	latency_count;
		u64	latency_sum_ms;
		u64	connections_total;
		u32	connections_active;
		u64	control_requests[2];	/**< runtime control requests: [0] ok, [1] error */
		/** @brief Counters of one device. */
		struct net_device_metrics_t
		{
			u64	requests;
			u64	responses[NET_RESULT_COUNT];
			u64	exceptions[16];		/**< by exception code */
			u64	faults[FAULT_KIND_COUNT];	/**< fired, by fault kind */
		} device[NET_OBS_MAX_DEVICES];
	} metrics;

	/** @brief Runtime control endpoint state. */
	struct net_control_t
	{
		bool	enabled;	/**< `/api/` answers; set by the server after net_obs_init() */
		bool	tcp_like;	/**< transport has connections: the disconnect fault is valid */
		bool	mbap;		/**< transport frames have no checksum: corrupt checksum is invalid */
		bool	ui;		/**< `GET /` serves the web page; set after net_obs_init() */
		/** HTTP bind address (the Host check accepts it unless it is a wildcard). */
		char	bind[NET_OBS_HOST_MAX];
		/** Extra Host names `/api/` and the page accept, comma-separated, case-insensitive. */
		char	allowed_hosts[NET_OBS_HOSTS_MAX];
	} control;

	/** @brief HTTP endpoint listener and its connections. */
	struct net_http_t
	{
		os_socket_t	listen_fd;	/**< OS_SOCKET_INVALID when the endpoint is off */
		/** @brief One HTTP client connection. */
		struct net_http_conn_t
		{
			os_socket_t	fd;	/**< OS_SOCKET_INVALID when the slot is free */
			u64	deadline_ms;
			u32	in_len;
			u32	out_len;
			u32	out_sent;	/**< bytes sent of `out` followed by `ext` */
			const u8* ext;		/**< static body sent after `out` (the web page), or NULL */
			u32	ext_len;
			u32	head_len;	/**< 0 until the request head is complete */
			u32	body_len;	/**< Content-Length */
			bool	writing;
			char	in[NET_OBS_HTTP_IN + NET_OBS_HTTP_BODY + 1];
			char	out[NET_OBS_HTTP_OUT];
		} conns[NET_OBS_HTTP_CONNS];
	} http;
};

/**
 * @brief Resets `obs` and, when `http_bind` is not NULL, opens the HTTP endpoint.
 *
 * @param obs Observability state to initialize.
 * @param transport Mode name used in logs and metrics; borrowed.
 * @param devices Served devices; borrowed.
 * @param device_count Number of devices, at most NET_OBS_MAX_DEVICES.
 * @param http_bind IPv4 or IPv6 literal for the endpoint, or NULL for no endpoint.
 * @param http_port Endpoint TCP port.
 * @return 0, or -1 if the endpoint could not listen (after a WARN).
 */
s32	net_obs_init(net_obs_t* obs, const char* transport, device_t** devices, u32 device_count,
		     const char* http_bind, u16 http_port);

/**
 * @brief Checks a comma-separated Host name list for `http_allowed_hosts`.
 *
 * At most NET_OBS_HOSTS_COUNT names of letters, digits, `.`, `-` and `_`, in fewer than
 * NET_OBS_HOSTS_MAX characters in all. NULL and "" are valid (no names).
 */
bool	net_obs_hosts_valid(const char* list);

/** @brief Closes the endpoint and its connections. Idempotent; NULL is ignored. */
void	net_obs_close(net_obs_t* obs);

/**
 * @brief Records one decoded request and logs it at DEBUG (text or JSON).
 *
 * @param obs Observability state; NULL records and logs nothing.
 * @param peer Client description for logs, or NULL for a byte stream.
 * @param dev Addressed device, or NULL (unknown unit, broadcast).
 * @param unit Unit id from the frame.
 * @param fc Function code from the request.
 * @param resp Response PDU (only read when @p resp_len > 0).
 * @param resp_len Response PDU length; 0 = no response.
 * @param decision Fault decision for the response, or NULL for none.
 */
void	net_obs_request(net_obs_t* obs, const char* peer, const device_t* dev, u8 unit, u8 fc,
			const u8* resp, u32 resp_len, const fault_decision_t* decision);

/**
 * @brief Shows a frame to the frame hook of @p obs, which may rewrite it in place.
 *
 * @param obs Observability state; NULL or no hook returns @p len unchanged.
 * @param dir Direction of the frame.
 * @param peer Client description for logs, or NULL for a byte stream.
 * @param data Frame bytes, with room for @p capacity.
 * @param len Frame length.
 * @param capacity Most bytes @p data can hold.
 * @return The (possibly new) length; 0 when the hook dropped the frame or returned more than
 *         @p capacity (WARN).
 */
u32	net_frame_trace(net_obs_t* obs, net_frame_dir_t dir, const char* peer, u8* data, u32 len,
			u32 capacity);

/**
 * @brief Copies a received frame into @p out (room for @p capacity, at least @p len) and shows it to
 *        the frame hook there, so the hook never overwrites bytes of the receive buffer.
 *
 * @return The length of the frame in @p out, 0 when dropped.
 */
u32	net_frame_rx(net_obs_t* obs, const char* peer, const u8* frame, u32 len, u8* out, u32 capacity);

/** @brief Counts a frame that was rejected before reaching a device. NULL is ignored. */
void	net_obs_rejected(net_obs_t* obs);

/** @brief Counts an accepted (`opened` true) or closed client connection. NULL is ignored. */
void	net_obs_connection(net_obs_t* obs, bool opened);

/**
 * @brief Renders the metrics in the Prometheus text exposition format (version 0.0.4).
 * @return Length written, or 0 if @p size is too small.
 */
u32	net_obs_render_metrics(const net_obs_t* obs, char* out, u32 size);

/**
 * @brief Handles the bytes in `conn->in` (`in_len`, as a socket read left them).
 *
 * Once the head and the Content-Length body are complete it builds the response into
 * `conn->out` and sets `writing`. Split from the socket code so a fuzzer can call it.
 * @return false if the connection must be closed (head too large).
 */
bool	net_obs_http_feed(net_obs_t* obs, struct net_http_conn_t* conn);

/**
 * @brief os_wait() for a transport: adds the endpoint's fds, waits, then serves the endpoint.
 *
 * `fds` and its revents are exactly as with os_wait(); the endpoint only lowers the timeout to
 * its own deadlines. `count` plus the endpoint's fds must fit NET_OBS_POLL_MAX.
 * In a group (see net_poll_group_t) the gather phase returns -1 and the replay phase the number
 * of ready entries.
 * @return os_wait()'s result for the whole set (negative on error).
 */
int	net_poll(net_obs_t* obs, os_wait_entry_t* fds, u32 count, int timeout_ms);

/**
 * @brief True if a transport's step should return NET_STEP_DONE for its stop flag.
 *
 * In the replay phase of a group the wait already returned events for this server, so they are
 * served even when a stop arrived meanwhile (as a lone server does); the next gather ends it.
 * @param obs The server's observability state; may be NULL.
 * @param stop The transport's stop flag.
 */
bool	net_stop_requested(const net_obs_t* obs, bool stop);

/** @brief True while `obs` is in the gather phase, where net_poll() returns -1 without an error. */
bool	net_poll_gathering(const net_obs_t* obs);

#endif
