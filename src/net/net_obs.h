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

#include "modbus/pdu.h"
#include "vlib/c_types.h"
#include <stdbool.h>

struct pollfd;

enum
{
	NET_OBS_MAX_DEVICES	= 16,	/**< same as SERVER_MAX_DEVICES (checked in server.c) */
	NET_OBS_LATENCY_BUCKETS	= 12,
	NET_OBS_HTTP_CONNS	= 4,	/**< concurrent HTTP clients; others wait in the backlog */
	NET_OBS_HTTP_IN		= 1024,	/**< request head limit */
	NET_OBS_HTTP_OUT	= 32768,	/**< response limit, enough for 16 fully used devices */
	NET_OBS_HTTP_TIMEOUT_MS	= 2000,	/**< a connection must be done within this */
	NET_OBS_POLL_MAX	= 64,	/**< transport fds plus endpoint fds per poll */
};

/** @brief What happened to one request. */
typedef enum net_result_t net_result_t;
enum net_result_t
{
	NET_RESULT_OK,		/**< normal response */
	NET_RESULT_EXCEPTION,	/**< Modbus exception response (from the device or a fault) */
	NET_RESULT_NONE,	/**< no response by protocol: broadcast, unknown serial unit, listen-only */
	NET_RESULT_DROPPED,	/**< response suppressed by a drop or disconnect fault */
	NET_RESULT_COUNT,
};

/** @brief Upper bounds of the response latency histogram, in milliseconds. */
extern const u32 NET_OBS_LATENCY_MS[NET_OBS_LATENCY_BUCKETS];

/**
 * @brief Observability state of one server.
 *
 * Zero-initialized means "record nothing, serve nothing" until net_obs_init().
 */
typedef struct net_obs_t net_obs_t;
struct net_obs_t
{
	const char*	transport;	/**< mode name for logs and metrics, e.g. "tcp" */
	device_t**	devices;	/**< borrowed; metrics are kept per index */
	u32		device_count;
	u64		start_unix_s;	/**< wall-clock start, for mogbus_start_time_seconds */
	bool		ready;		/**< serving; false before start and once stopping */

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
		struct net_device_metrics_t
		{
			u64	requests;
			u64	responses[NET_RESULT_COUNT];
			u64	exceptions[16];		/**< by exception code */
			u64	faults[FAULT_KIND_COUNT];	/**< fired, by fault kind */
		} device[NET_OBS_MAX_DEVICES];
	} metrics;

	struct net_http_t
	{
		int	listen_fd;	/**< -1 when the endpoint is off */
		struct net_http_conn_t
		{
			int	fd;	/**< -1 when the slot is free */
			u64	deadline_ms;
			u32	in_len;
			u32	out_len;
			u32	out_sent;
			bool	writing;
			char	in[NET_OBS_HTTP_IN];
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
 * @brief poll() for a transport: adds the endpoint's fds, polls, then serves the endpoint.
 *
 * `fds` and its revents are exactly as with poll(); the endpoint only lowers the timeout to
 * its own deadlines. `count` plus the endpoint's fds must fit NET_OBS_POLL_MAX.
 * @return poll()'s result for the whole set (negative on error).
 */
int	net_poll(net_obs_t* obs, struct pollfd* fds, u32 count, int timeout_ms);

#endif
