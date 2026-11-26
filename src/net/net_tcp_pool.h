/**
 * @file
 * @brief Shared TCP listener, accept backoff, idle sweep and poll table.
 */
#ifndef NET_TCP_POOL_H
#define NET_TCP_POOL_H

#include "vlib/c_types.h"
#include <stdbool.h>

typedef struct net_obs_t net_obs_t;
typedef struct net_tcp_conn_t net_tcp_conn_t;
typedef struct net_tcp_pool_t net_tcp_pool_t;

struct pollfd;

/** @brief Size of the peer text buffer. */
enum
{
	NET_TCP_PEER_SIZE = 64,
};

/**
 * @brief First member of every server's connection struct.
 *
 * The pool owns only the socket, activity stamp, peer string and accept bookkeeping.
 */
struct net_tcp_conn_t
{
	int		fd;		/**< -1 when the slot is free */
	u64		last_activity_ms;
	char		peer[NET_TCP_PEER_SIZE];
};

/**
 * @brief Caller-owned connection array: `conns` points at the first base member, `conn_size`
 *        is sizeof(the caller's connection struct).
 *
 * Callbacks keep framing/TLS concerns in the servers.
 */
struct net_tcp_pool_t
{
	net_tcp_conn_t*	conns;
	u32		conn_count;
	u32		conn_size;
	u32		idle_timeout_ms;
	int		listen_fd;
	u64		accept_backoff_until_ms;
	bool		accept_backoff_logged;
	net_obs_t*	obs;	/**< connection metrics; NULL records nothing */
	void*		ctx;
	void		(*on_accept)(void* ctx, net_tcp_conn_t* conn, u64 now_ms);
	void		(*on_close)(void* ctx, net_tcp_conn_t* conn);
	bool		(*skip_idle)(void* ctx, const net_tcp_conn_t* conn);
};

/** @brief Opens the shared nonblocking listener (backlog 64) and stores its fd. */
int	net_tcp_pool_listen(net_tcp_pool_t* pool, const char* bind_address, u16 port);

/** @brief Accepts until EAGAIN, applying the shared backoff and connection setup. */
void	net_tcp_pool_accept(net_tcp_pool_t* pool);

void	net_tcp_pool_close_conn(net_tcp_pool_t* pool, net_tcp_conn_t* conn);
void	net_tcp_pool_close_idle(net_tcp_pool_t* pool, u64 now_ms);

/**
 * @brief Closes every active connection (through on_close) and the listener.
 *
 * Safe to call repeatedly: closed slots and listeners are left at -1.
 */
void	net_tcp_pool_close_all(net_tcp_pool_t* pool);

/**
 * @brief Fills entry 0 with the listener and one entry per active connection.
 *
 * Callers refine events and add their own deadlines afterwards.
 */
u32	net_tcp_pool_build_poll(net_tcp_pool_t* pool, struct pollfd* fds,
				net_tcp_conn_t** owners, u64 now_ms, int* timeout);

#endif
