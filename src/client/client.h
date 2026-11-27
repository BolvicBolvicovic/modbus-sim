/**
 * @file
 * @brief Modbus client transport: one request/reply exchange over TCP, UDP or a serial line.
 *
 * A client talks to an endpoint over the wire only; it shares nothing with the servers. It owns
 * its memory (a private arena), connects lazily, reconnects on the next call after a connection
 * error, and blocks with a deadline. One thread at a time per client.
 */
#ifndef CLIENT_H
#define CLIENT_H

#include "os/os_serial.h"
#include "vlib/c_types.h"
#include <stdbool.h>

typedef enum client_mode_t client_mode_t;
typedef enum client_status_t client_status_t;
typedef struct client_options_t client_options_t;
typedef struct client_t client_t;

/** @brief Framing and carrier of a client. */
enum client_mode_t
{
	CLIENT_TCP,
	CLIENT_UDP,
	CLIENT_RTU_TCP,
	CLIENT_RTU_UDP,
	CLIENT_ASCII_TCP,
	CLIENT_ASCII_UDP,
	CLIENT_RTU_SERIAL,
	CLIENT_ASCII_SERIAL,
};

/** @brief Outcome of a client call. */
enum client_status_t
{
	CLIENT_OK,
	CLIENT_INVALID,		/**< bad argument or options */
	CLIENT_NOMEM,
	CLIENT_UNSUPPORTED,	/**< TLS, or serial lines on a platform without them */
	CLIENT_IO,		/**< connect, send or receive failed, or the peer closed */
	CLIENT_TIMEOUT,		/**< no complete reply before the deadline (after the retries) */
	CLIENT_EXCEPTION,	/**< the server answered with a Modbus exception; see client_exception() */
	CLIENT_FRAME,		/**< malformed reply, or a reply that does not answer the request */
};

/** @brief Client settings; strings are copied by client_new(). */
struct client_options_t
{
	client_mode_t	mode;
	const char*	host;		/**< IP literal or "localhost"; NULL = 127.0.0.1 */
	u16		port;
	const char*	path;		/**< serial modes */
	u32		baud;		/**< serial modes; 0 keeps the device's speed */
	u32		data_bits;	/**< serial modes: 7 or 8 */
	parity_t	parity;
	u32		stop_bits;	/**< serial modes: 1 or 2 */
	bool		tls;		/**< not supported: client_new() fails */
	u32		timeout_ms;	/**< per attempt; 0 = 1000 */
	u32		retries;	/**< extra attempts after a timeout */
};

/**
 * @brief Creates a client; nothing is connected yet.
 *
 * With no client to own the message, a failure leaves its text per thread
 * (client_detached_error()).
 * @param options Settings.
 * @param out Receives the client, NULL on failure.
 * @return CLIENT_OK, CLIENT_INVALID, CLIENT_NOMEM or CLIENT_UNSUPPORTED.
 */
client_status_t	client_new(const client_options_t* options, client_t** out);

/** @brief Closes the connection and releases the client's memory; NULL is ignored. */
void		client_free(client_t* c);

/** @brief Connects now (a no-op when connected); the text of a failure is client_error(). */
client_status_t	client_connect(client_t* c);

/**
 * @brief Sends the PDU `req` (function code first) to `unit` and waits for the reply PDU.
 *
 * Reconnects first when needed and repeats a timed-out attempt `retries` times. Unit 0 on the
 * serial/RTU/ASCII modes is a broadcast: the frame is sent, nothing is awaited, *reply_len is 0.
 * On CLIENT_EXCEPTION the reply is still copied and client_exception() holds the code.
 */
client_status_t	client_request(client_t* c, u8 unit, const u8* req, u32 req_len,
			       u8* reply, u32 reply_cap, u32* reply_len);

/** @brief Text of the last failed call on this client; "" before the first failure. */
const char*	client_error(const client_t* c);

/** @brief Exception code of the last CLIENT_EXCEPTION. */
u8		client_exception(const client_t* c);

/** @brief Sets the text of client_error() for a failure detected above the transport. */
void		client_set_error(client_t* c, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

/** @brief Text of the last client_new() or mogbus_probe() failure of the calling thread. */
const char*	client_detached_error(void);

/** @brief Sets the per-thread text read by client_detached_error(); "" clears it. */
void		client_detached_errorf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
