/* mogbus public C API: a Modbus device simulator you can embed in a program.
 *
 * This is the only header a library user needs. It uses only <stdint.h>,
 * <stdbool.h> and <stddef.h> types and every symbol starts with mogbus_ or
 * MOGBUS_. Link with libmogbus.so or libmogbus.a (see docs/library.md).
 *
 * Model: a mogbus_t context owns the devices and servers you create. Devices
 * come from a config file (mogbus_load_config) or are built in code
 * (mogbus_add_device + mogbus_device_add_point). A server exposes all devices
 * of the context over one transport (TCP, RTU, ASCII, ...), and the host drives
 * it with mogbus_server_step() from its own loop, or hands the thread over with
 * mogbus_server_run().
 *
 * Rules:
 *  - Not thread-safe. One thread at a time may call into a context: the thread
 *    that steps a server is the thread that must touch its devices. The only
 *    call that may come from another thread or a signal handler is
 *    mogbus_server_stop().
 *  - Nothing is allocated with malloc; memory comes from an mmap-backed arena
 *    owned by the context and is released by mogbus_destroy(). Devices live as
 *    long as the context (there is no per-device removal), so a pointer to a
 *    device or a context stays valid until mogbus_destroy().
 *  - A mogbus_server_t is valid until mogbus_server_close() (or
 *    mogbus_destroy(), which closes every server of the context).
 *  - Strings you pass in (paths, bind address, TLS files, names) are copied
 *    during the call; you may free them afterwards. Strings the library returns
 *    (mogbus_version, mogbus_server_pty_name) are borrowed from the library or
 *    the server and must not be freed or modified.
 *  - The library never installs signal handlers. A TLS server writes to its
 *    sockets through OpenSSL, which can raise SIGPIPE when a client vanishes:
 *    a host that uses TLS must ignore SIGPIPE (signal(SIGPIPE, SIG_IGN)).
 *  - err/err_size arguments are optional (NULL or 0 discards the message).
 *    When given, the message is always NUL-terminated.
 */
#ifndef MOGBUS_H
#define MOGBUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOGBUS_API __attribute__((visibility("default")))

#define MOGBUS_VERSION_MAJOR	0
#define MOGBUS_VERSION_MINOR	0
#define MOGBUS_VERSION_PATCH	1

/* Same as the MOGBUS_VERSION_* macros, as "major.minor.patch". */
MOGBUS_API const char*	mogbus_version(void);

/* A context can hold this many devices and servers at once. */
#define MOGBUS_MAX_DEVICES	16
#define MOGBUS_MAX_SERVERS	8

typedef struct mogbus_t		mogbus_t;
typedef struct mogbus_device_t	mogbus_device_t;
typedef struct mogbus_server_t	mogbus_server_t;

/* ---- Context ---- */

/* Returns NULL if the arena cannot be mapped. */
MOGBUS_API mogbus_t*	mogbus_create(void);

/* Closes every server created from the context and releases all memory,
 * including devices and the context itself. NULL is ignored. */
MOGBUS_API void		mogbus_destroy(mogbus_t* ctx);

/* Reads a config file (see docs/config-reference.md): adds its devices to the
 * context and remembers its (server ...) section for
 * mogbus_server_create_from_config(). Returns false with a message in err and
 * leaves the context unchanged on failure (bad file, too many devices, or a
 * unit id that already exists). May be called again; a later file replaces
 * the remembered server section. */
MOGBUS_API bool		mogbus_load_config(mogbus_t* ctx, const char* path, char* err, size_t err_size);

/* ---- Devices ---- */

typedef enum mogbus_space_t
{
	MOGBUS_COIL,		/* read/write bit     (FC 01, 05, 15) */
	MOGBUS_DISCRETE,	/* read-only bit      (FC 02)         */
	MOGBUS_INPUT,		/* read-only 16 bits  (FC 04)         */
	MOGBUS_HOLDING,		/* read/write 16 bits (FC 03, 06, 16) */
	MOGBUS_SPACE_COUNT,
} mogbus_space_t;

/* How a number is stored in 16-bit registers. Use MOGBUS_TYPE_BOOL for coils
 * and discrete inputs, and only for them. The 32-bit types use two registers. */
typedef enum mogbus_value_type_t
{
	MOGBUS_TYPE_U16,
	MOGBUS_TYPE_S16,
	MOGBUS_TYPE_U32,
	MOGBUS_TYPE_S32,
	MOGBUS_TYPE_F32,
	MOGBUS_TYPE_BOOL,
} mogbus_value_type_t;

/* For 32-bit values: which register holds the high half. */
typedef enum mogbus_word_order_t
{
	MOGBUS_WORDS_HIGH_FIRST,
	MOGBUS_WORDS_LOW_FIRST,
} mogbus_word_order_t;

typedef enum mogbus_point_kind_t
{
	MOGBUS_POINT_STATIC,	/* `value` until a client writes another one */
	MOGBUS_POINT_RAMP,	/* follows `ramp` over time (registers only) */
} mogbus_point_kind_t;

/* What a ramp does when it reaches its end. */
typedef enum mogbus_ramp_mode_t
{
	MOGBUS_RAMP_WRAP,	/* jump back to `from` */
	MOGBUS_RAMP_BOUNCE,	/* turn around and walk back */
	MOGBUS_RAMP_HOLD,	/* stay on the last value */
} mogbus_ramp_mode_t;

/* One data point. Zero-initialise it, then set the fields you need: a zeroed
 * point is a static 0 of type U16 in the coil table, so set `type` for any
 * space but holding/input. A point occupies 1 address, or 2 for 32-bit types.
 * Addresses may not overlap and must fit below 65536. */
typedef struct mogbus_point_t
{
	mogbus_space_t		space;
	uint32_t		address;
	mogbus_value_type_t	type;
	mogbus_word_order_t	word_order;
	mogbus_point_kind_t	kind;
	double			value;		/* MOGBUS_POINT_STATIC; bits: nonzero = on */
	struct
	{
		double			from, to;
		double			step;		/* > 0; the direction comes from from/to */
		uint32_t		every_ms;	/* > 0 */
		mogbus_ramp_mode_t	mode;
	} ramp;					/* MOGBUS_POINT_RAMP */
} mogbus_point_t;

/* Adds a device to the context. `name` (NULL = "unnamed") is copied; at most
 * 63 bytes. Fails on a duplicate unit id or when MOGBUS_MAX_DEVICES is reached.
 * Machines and faults are available only through config files for now. */
MOGBUS_API mogbus_device_t*	mogbus_add_device(mogbus_t* ctx, uint8_t unit_id, const char* name,
						  char* err, size_t err_size);

/* The device with this unit id, or NULL. Includes devices loaded from config. */
MOGBUS_API mogbus_device_t*	mogbus_find_device(mogbus_t* ctx, uint8_t unit_id);

/* Enumeration: devices are numbered 0..count-1 in the order they were added
 * (devices of a config file keep file order). mogbus_device_at() returns NULL
 * for an index out of range. */
MOGBUS_API size_t		mogbus_device_count(const mogbus_t* ctx);
MOGBUS_API mogbus_device_t*	mogbus_device_at(mogbus_t* ctx, size_t index);

/* Fault kinds a config file can attach to a device (see docs/config-reference.md). */
typedef enum mogbus_fault_kind_t
{
	MOGBUS_FAULT_DELAY,
	MOGBUS_FAULT_DROP,
	MOGBUS_FAULT_EXCEPTION,
	MOGBUS_FAULT_CORRUPT,
	MOGBUS_FAULT_DISCONNECT,
	MOGBUS_FAULT_RESTART,
	MOGBUS_FAULT_SENSOR_DISCONNECT,
	MOGBUS_FAULT_KIND_COUNT,
} mogbus_fault_kind_t;

/* A snapshot of what a device is made of. Points are counted per address
 * space (index with mogbus_space_t); faults are indexed by mogbus_fault_kind_t
 * and only those configured on the device itself are listed. A fault fires on
 * `percent` of the responses, or by a fixed schedule when `scheduled` is set
 * (percent is then meaningless). delay_ms is set for MOGBUS_FAULT_DELAY and
 * exception_code for MOGBUS_FAULT_EXCEPTION. */
typedef struct mogbus_device_info_t
{
	const char*	name;			/* borrowed from the device */
	uint8_t		unit_id;
	uint32_t	point_count[MOGBUS_SPACE_COUNT];
	struct
	{
		bool		present;
		bool		scheduled;
		uint32_t	percent;
		uint32_t	delay_ms;
		uint8_t		exception_code;
	} faults[MOGBUS_FAULT_KIND_COUNT];
} mogbus_device_info_t;

MOGBUS_API bool		mogbus_device_info(const mogbus_device_t* dev, mogbus_device_info_t* out);

/* Returns false with a message in err for an invalid point (type does not fit
 * the space, overlapping or out-of-range address, bad ramp, ...). */
MOGBUS_API bool		mogbus_device_add_point(mogbus_device_t* dev, const mogbus_point_t* point,
						char* err, size_t err_size);

/* Raw table access (ramps are advanced to the current time first). `count`
 * consecutive addresses are read or written, each a 16-bit register; bit
 * spaces use 0 or 1 and writes of other values fail. Both fail, changing
 * nothing, when any address is undefined or out of range.
 * mogbus_device_write() works on every space, including the read-only ones
 * (the host plays the physical side), and a written ramp becomes static. Like
 * a client write it fails on a point a machine drives or that the host claimed
 * with mogbus_device_set_value(), and it runs matching machine on-write
 * handlers. */
MOGBUS_API bool		mogbus_device_read(mogbus_device_t* dev, mogbus_space_t space, uint32_t address,
					   uint32_t count, uint16_t* out);
MOGBUS_API bool		mogbus_device_write(mogbus_device_t* dev, mogbus_space_t space, uint32_t address,
					    uint32_t count, const uint16_t* in);

/* Decoded access to the point that starts at `address` (the point's first
 * address, not an address inside it). Bit points read as 0.0 / 1.0.
 *
 * mogbus_device_set_value() is the host side of a point, like a machine
 * output: it works on every space (including read-only ones), but the point
 * becomes host-owned. Ramps stop and client writes to it get Modbus exception
 * 02 until the device restarts. Use mogbus_device_write() to change a value
 * the way a client would. Both return false if no point starts at `address`. */
MOGBUS_API bool		mogbus_device_get_value(mogbus_device_t* dev, mogbus_space_t space,
						uint32_t address, double* out);
MOGBUS_API bool		mogbus_device_set_value(mogbus_device_t* dev, mogbus_space_t space,
						uint32_t address, double value);

/* ---- Server ---- */

typedef enum mogbus_mode_t
{
	MOGBUS_MODE_TCP,		/* Modbus TCP (MBAP), optionally TLS */
	MOGBUS_MODE_RTU_PTY,		/* RTU on a new PTY; see mogbus_server_pty_name() */
	MOGBUS_MODE_RTU_DEVICE,		/* RTU on the serial device `path` */
	MOGBUS_MODE_RTU_TCP,		/* RTU frames over TCP */
	MOGBUS_MODE_ASCII_PTY,
	MOGBUS_MODE_ASCII_DEVICE,
	MOGBUS_MODE_ASCII_TCP,		/* ASCII frames over TCP */
	MOGBUS_MODE_RTU_FD,		/* RTU on an open byte-stream fd you own */
	MOGBUS_MODE_ASCII_FD,		/* ASCII on an open byte-stream fd you own */
	MOGBUS_MODE_UDP,		/* Modbus UDP: one MBAP ADU per datagram */
	MOGBUS_MODE_ASCII_UDP,		/* Modbus ASCII: one CRLF-terminated frame per datagram */
	MOGBUS_MODE_RTU_UDP,		/* RTU frames, one per UDP datagram */
} mogbus_mode_t;

typedef enum mogbus_parity_t
{
	MOGBUS_PARITY_NONE,
	MOGBUS_PARITY_EVEN,
	MOGBUS_PARITY_ODD,
	MOGBUS_PARITY_MARK,
	MOGBUS_PARITY_SPACE,
} mogbus_parity_t;

typedef enum mogbus_tls_version_t
{
	MOGBUS_TLS_1_2,
	MOGBUS_TLS_1_3,
} mogbus_tls_version_t;

/* Fields a mode does not use are ignored. Always start from
 * mogbus_server_options_defaults(), then change what you need. */
typedef struct mogbus_server_options_t
{
	mogbus_mode_t	mode;
	const char*	bind_address;		/* TCP-family and UDP modes; "0.0.0.0" opts in to remote access */
	uint16_t	port;			/* TCP-family and UDP modes; use 802 by convention with TLS */
	const char*	path;			/* *_DEVICE modes */
	int		fd;			/* *_FD modes: yours, never closed by the library */
	uint32_t	seed;			/* fault dice seed; 0 = fixed default */
	uint32_t	idle_timeout_ms;	/* TCP-family modes; 0 = never */
	uint32_t	frame_gap_ms;		/* RTU modes: inter-frame gap override; 0 = auto */
	uint32_t	baud;			/* RTU timing and serial speed; 0 = unknown */
	bool		strict_t15;		/* RTU byte streams: discard frames with a t1.5 gap */
	bool		broadcast_execute;	/* RTU modes: run unit-0 requests (still no reply) */
	uint32_t	data_bits;		/* serial: 7 or 8 (RTU always uses 8) */
	mogbus_parity_t	parity;			/* serial */
	uint32_t	stop_bits;		/* serial: 1 or 2 */
	bool		tls_enabled;		/* MOGBUS_MODE_TCP only; needs a build with TLS */
	uint32_t	tls_handshake_timeout_ms;
	const char*	tls_certificate;	/* PEM file paths */
	const char*	tls_private_key;
	const char*	tls_ca_certificate;	/* NULL = do not verify clients */
	bool		tls_require_client_certificate;
	mogbus_tls_version_t tls_min_version;
	const char*	tls_ciphers_1_2;	/* NULL = OpenSSL default */
	const char*	tls_ciphersuites_1_3;	/* NULL = OpenSSL default */
} mogbus_server_options_t;

/* Same defaults as a config file: TCP on 127.0.0.1:1502, no idle timeout,
 * 8N1, TLS off (when enabled: handshake timeout 10000 ms, client certificates
 * required, TLS 1.2 minimum), fd -1. */
MOGBUS_API mogbus_server_options_t	mogbus_server_options_defaults(void);

/* Why a server could not be created (see mogbus_server_create()). */
typedef enum mogbus_error_t
{
	MOGBUS_OK,
	MOGBUS_ERR_INVALID,	/* bad options, no devices, or a limit reached */
	MOGBUS_ERR_SERIAL,	/* the serial device or PTY could not be opened */
	MOGBUS_ERR_LISTEN,	/* the listener could not start (port in use) or TLS setup failed */
} mogbus_error_t;

/* Starts serving every device the context has now (devices added later are
 * not served). Returns NULL with a message in err if the options are invalid
 * or the transport cannot start (port in use, serial device missing, TLS files
 * unreadable, no devices, MOGBUS_MAX_SERVERS reached). `status` is optional:
 * it receives MOGBUS_OK or the reason for the failure. */
MOGBUS_API mogbus_server_t*	mogbus_server_create(mogbus_t* ctx, const mogbus_server_options_t* options,
						     char* err, size_t err_size, mogbus_error_t* status);

/* Same, using the (server ...) section of the last mogbus_load_config(). */
MOGBUS_API mogbus_server_t*	mogbus_server_create_from_config(mogbus_t* ctx, char* err, size_t err_size,
								 mogbus_error_t* status);

/* The (server ...) section of the last mogbus_load_config(), as options. Strings
 * are borrowed from the context. Returns false if no config is loaded or the
 * build has no config support. */
MOGBUS_API bool		mogbus_config_server_options(const mogbus_t* ctx, mogbus_server_options_t* out);

/* Validates options exactly as mogbus_server_create() would (device path, fd,
 * serial line, string lengths, TLS files via a throwaway TLS context) without
 * opening a listener, serial device or PTY, so it also passes for a port that
 * is in use. Returns false with a message in err. */
MOGBUS_API bool		mogbus_server_check(const mogbus_t* ctx, const mogbus_server_options_t* options,
					    char* err, size_t err_size);

/* RTU timing for a baud rate and optional explicit frame gap: the Modbus t3.5
 * in microseconds (0 when baud is 0 = unknown) and the inter-frame gap in
 * milliseconds a server would use (frame_gap_ms if nonzero, else derived from
 * baud, else 20). Either output may be NULL. */
MOGBUS_API void		mogbus_rtu_timing(uint32_t baud, uint32_t frame_gap_ms,
					  uint32_t* t35_us, uint32_t* gap_ms);

enum
{
	MOGBUS_STEP_CONTINUE = 0,
	MOGBUS_STEP_DONE     = 1,	/* stopped, or the byte-stream peer went away */
	MOGBUS_STEP_ERROR    = 2,	/* poll failed */
};

/* Runs one poll iteration: accepts, reads, answers and writes whatever is
 * ready. max_wait_ms is the longest it may block: 0 = do not block, -1 = until
 * the server's next deadline (at most 1 s). Call it in a loop, interleaved with
 * your own work. Returns a MOGBUS_STEP_* value. */
MOGBUS_API int		mogbus_server_step(mogbus_server_t* server, int max_wait_ms);

/* Asks the server to stop: the next step returns MOGBUS_STEP_DONE. Async-signal-safe
 * and the only call that may be made from another thread or a signal handler. */
MOGBUS_API void		mogbus_server_stop(mogbus_server_t* server);

/* Steps until stopped or the byte-stream peer goes away, and closes the
 * transport. Returns 0 after a clean stop, 1 on a poll error. Still call
 * mogbus_server_close() afterwards to release the handle. */
MOGBUS_API int		mogbus_server_run(mogbus_server_t* server);

/* Closes the transport and releases the handle; NULL is ignored. Client
 * connections are dropped; fds you passed in are left open. */
MOGBUS_API void		mogbus_server_close(mogbus_server_t* server);

/* PTY slave path for the *_PTY modes, otherwise NULL. Borrowed from the server. */
MOGBUS_API const char*	mogbus_server_pty_name(const mogbus_server_t* server);

/* ---- Logging (process-wide) ---- */

typedef enum mogbus_log_level_t
{
	MOGBUS_LOG_DEBUG,
	MOGBUS_LOG_INFO,
	MOGBUS_LOG_WARN,
	MOGBUS_LOG_OFF,
} mogbus_log_level_t;

typedef void (*mogbus_log_fn)(void* user, mogbus_log_level_t level, const char* message);

/* Receives each log line (no trailing newline, truncated to 1023 bytes,
 * valid only during the call), synchronously, on the thread that logs. NULL
 * restores the default of writing timestamped INFO/WARN lines to stderr. */
MOGBUS_API void		mogbus_set_log_sink(mogbus_log_fn fn, void* user);

/* Lines below the level are dropped. Default: MOGBUS_LOG_INFO. */
MOGBUS_API void		mogbus_set_log_level(mogbus_log_level_t level);

#ifdef __cplusplus
}
#endif

#endif
