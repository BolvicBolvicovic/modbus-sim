/**
 * @file
 * @brief Public C API of the mogbus library: a Modbus device simulator to embed in a program.
 *
 * This is the only header a library user needs. It uses only <stdint.h>,
 * <stdbool.h> and <stddef.h> types and every symbol starts with mogbus_ or
 * MOGBUS_. Link with libmogbus.so or libmogbus.a (see docs/user/library.md).
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

typedef enum mogbus_space_t mogbus_space_t;
typedef enum mogbus_value_type_t mogbus_value_type_t;
typedef enum mogbus_word_order_t mogbus_word_order_t;
typedef enum mogbus_byte_order_t mogbus_byte_order_t;
typedef enum mogbus_point_kind_t mogbus_point_kind_t;
typedef enum mogbus_ramp_mode_t mogbus_ramp_mode_t;
typedef struct mogbus_point_t mogbus_point_t;
typedef enum mogbus_fault_kind_t mogbus_fault_kind_t;
typedef struct mogbus_device_info_t mogbus_device_info_t;
typedef enum mogbus_mode_t mogbus_mode_t;
typedef enum mogbus_parity_t mogbus_parity_t;
typedef enum mogbus_tls_version_t mogbus_tls_version_t;
typedef struct mogbus_server_options_t mogbus_server_options_t;
typedef enum mogbus_error_t mogbus_error_t;
typedef enum mogbus_log_level_t mogbus_log_level_t;
typedef enum mogbus_log_format_t mogbus_log_format_t;
/** @brief A context: owns devices, servers and their memory. */
typedef struct mogbus_t		mogbus_t;
/** @brief A simulated device (one unit id), owned by its context. */
typedef struct mogbus_device_t	mogbus_device_t;
/** @brief A running server: one transport serving the devices of a context. */
typedef struct mogbus_server_t	mogbus_server_t;

#ifdef __cplusplus
extern "C" {
#endif

#define MOGBUS_API __attribute__((visibility("default")))

#define MOGBUS_VERSION_MAJOR	0
#define MOGBUS_VERSION_MINOR	0
#define MOGBUS_VERSION_PATCH	1

/**
 * @brief Returns the library version.
 * @return "major.minor.patch", the same as the MOGBUS_VERSION_* macros.
 */
MOGBUS_API const char*	mogbus_version(void);

/**
 * @brief Maximum number of devices in one context.
 */
#define MOGBUS_MAX_DEVICES	16
#define MOGBUS_MAX_SERVERS	8	/**< Maximum number of servers in one context. */

/* ---- Context ---- */

/**
 * @brief Creates an empty context.
 * @return The context, or NULL if its memory cannot be mapped.
 */
MOGBUS_API mogbus_t*	mogbus_create(void);

/**
 * @brief Closes every server of the context and releases all its memory, devices included.
 * @param ctx Context to destroy; NULL is ignored.
 */
MOGBUS_API void		mogbus_destroy(mogbus_t* ctx);

/**
 * @brief Loads a config file into the context.
 *
 * Adds the file's devices and remembers its (server ...) section for
 * mogbus_server_create_from_config(). On failure (bad file, too many devices, a unit id that
 * already exists) the context is unchanged. A later call replaces the remembered server section.
 * See docs/user/config-reference.md.
 *
 * @param ctx Context.
 * @param path Config file path.
 * @param err Buffer for an error message; may be NULL.
 * @param err_size Size of @p err; 0 discards the message.
 * @return true on success.
 */
MOGBUS_API bool		mogbus_load_config(mogbus_t* ctx, const char* path, char* err, size_t err_size);

/* ---- Devices ---- */

/** @brief The four Modbus data tables. */
enum mogbus_space_t
{
	MOGBUS_COIL,		/**< read/write bit     (FC 01, 05, 15) */
	MOGBUS_DISCRETE,	/**< read-only bit      (FC 02)         */
	MOGBUS_INPUT,		/**< read-only 16 bits  (FC 04)         */
	MOGBUS_HOLDING,		/**< read/write 16 bits (FC 03, 06, 16) */
	MOGBUS_SPACE_COUNT,	/**< number of tables */
};

/**
 * @brief How a number is stored in 16-bit registers.
 *
 * Use MOGBUS_TYPE_BOOL for coils and discrete inputs, and only for them. The 32-bit types use
 * two registers and the 64-bit types four. MOGBUS_TYPE_STRING is text: `length` registers of two
 * ASCII bytes each, holding or input tables only, static only. Values are doubles inside the
 * library, so 64-bit integers above 2^53 lose precision through mogbus_device_get_value() and
 * ramps; raw register access is exact. New types are appended, so values stay stable.
 */
enum mogbus_value_type_t
{
	MOGBUS_TYPE_U16,
	MOGBUS_TYPE_S16,
	MOGBUS_TYPE_U32,
	MOGBUS_TYPE_S32,
	MOGBUS_TYPE_F32,
	MOGBUS_TYPE_BOOL,
	MOGBUS_TYPE_U64,
	MOGBUS_TYPE_S64,
	MOGBUS_TYPE_F64,
	MOGBUS_TYPE_STRING,
};

/** @brief For 32- and 64-bit values: which register holds the most significant part. */
enum mogbus_word_order_t
{
	MOGBUS_WORDS_HIGH_FIRST,
	MOGBUS_WORDS_LOW_FIRST,
};

/** @brief Whether the two bytes inside every register of a point are swapped. */
enum mogbus_byte_order_t
{
	MOGBUS_BYTES_BIG,	/**< high byte first, as on the wire */
	MOGBUS_BYTES_LITTLE,	/**< bytes swapped inside every register */
};

/** @brief How a point's value behaves over time. */
enum mogbus_point_kind_t
{
	MOGBUS_POINT_STATIC,	/**< `value` until a client writes another one */
	MOGBUS_POINT_RAMP,	/**< follows `ramp` over time (registers only) */
};

/** @brief What a ramp does when it reaches its end. */
enum mogbus_ramp_mode_t
{
	MOGBUS_RAMP_WRAP,	/**< jump back to `from` */
	MOGBUS_RAMP_BOUNCE,	/**< turn around and walk back */
	MOGBUS_RAMP_HOLD,	/**< stay on the last value */
};

/**
 * @brief One data point.
 *
 * Zero-initialise it, then set the fields you need: a zeroed point is a static 0 of type U16
 * in the coil table, so set `type` for any space but holding/input. A point occupies 1
 * address, 2 for 32-bit types, 4 for 64-bit types and `length` for strings. Addresses may not
 * overlap and must fit below 65536.
 */
struct mogbus_point_t
{
	mogbus_space_t		space;		/**< table the point lives in */
	uint32_t		address;	/**< first address, below 65536 */
	mogbus_value_type_t	type;		/**< register encoding */
	mogbus_word_order_t	word_order;	/**< 32- and 64-bit types only */
	mogbus_point_kind_t	kind;		/**< static or ramp */
	double			value;		/**< MOGBUS_POINT_STATIC; bits: nonzero = on */
	/** @brief Ramp parameters, used when `kind` is MOGBUS_POINT_RAMP. */
	struct
	{
		double			from, to;	/**< start and end values */
		double			step;		/**< > 0; the direction comes from from/to */
		uint32_t		every_ms;	/**< > 0 */
		mogbus_ramp_mode_t	mode;		/**< what happens at the end */
	} ramp;					/**< MOGBUS_POINT_RAMP */
	mogbus_byte_order_t	byte_order;	/**< swaps the bytes of every register; not for bits */
	uint32_t		length;		/**< MOGBUS_TYPE_STRING: registers, 1 to 123 */
	const char*		text;		/**< MOGBUS_TYPE_STRING: initial text, at most 2*length bytes,
						     NUL padded; copied; NULL means empty */
	bool			read_only;	/**< coils and holding registers only: Modbus writes are
						     refused with exception 02; mogbus_device_write() and
						     the set_value calls still work */
};

/**
 * @brief Adds an empty device to the context.
 *
 * Machines and faults are only available through config files.
 *
 * @param ctx Context.
 * @param unit_id Modbus unit id; must not be in use.
 * @param name Device name, copied, at most 63 bytes; NULL means "unnamed".
 * @param err Buffer for an error message; may be NULL.
 * @param err_size Size of @p err; 0 discards the message.
 * @return The device, or NULL on a duplicate unit id or when MOGBUS_MAX_DEVICES is reached.
 */
MOGBUS_API mogbus_device_t*	mogbus_add_device(mogbus_t* ctx, uint8_t unit_id, const char* name,
						  char* err, size_t err_size);

/**
 * @brief Finds a device by unit id, including devices loaded from config.
 * @param ctx Context.
 * @param unit_id Unit id to look up.
 * @return The device, or NULL.
 */
MOGBUS_API mogbus_device_t*	mogbus_find_device(mogbus_t* ctx, uint8_t unit_id);

/**
 * @brief Returns the number of devices in the context.
 *
 * Devices are numbered 0..count-1 in the order they were added; devices of a config file keep
 * file order.
 * @param ctx Context.
 * @return Number of devices.
 */
MOGBUS_API size_t		mogbus_device_count(const mogbus_t* ctx);
/**
 * @brief Returns the device at an index of the enumeration order (see mogbus_device_count()).
 * @param ctx Context.
 * @param index 0..count-1.
 * @return The device, or NULL if @p index is out of range.
 */
MOGBUS_API mogbus_device_t*	mogbus_device_at(mogbus_t* ctx, size_t index);

/** @brief Fault kinds a config file can attach to a device (see docs/user/config-reference.md). */
enum mogbus_fault_kind_t
{
	MOGBUS_FAULT_DELAY,
	MOGBUS_FAULT_DROP,
	MOGBUS_FAULT_EXCEPTION,
	MOGBUS_FAULT_CORRUPT,
	MOGBUS_FAULT_DISCONNECT,
	MOGBUS_FAULT_RESTART,
	MOGBUS_FAULT_SENSOR_DISCONNECT,
	MOGBUS_FAULT_KIND_COUNT,
};

/**
 * @brief A snapshot of what a device is made of.
 *
 * Points are counted per address space (index with mogbus_space_t); faults are indexed by
 * mogbus_fault_kind_t and only those configured on the device itself are listed. A fault fires
 * on `percent` of the responses, or by a fixed schedule when `scheduled` is set (percent is
 * then meaningless). delay_ms is set for MOGBUS_FAULT_DELAY and exception_code for
 * MOGBUS_FAULT_EXCEPTION.
 */
struct mogbus_device_info_t
{
	const char*	name;			/**< borrowed from the device */
	uint8_t		unit_id;		/**< Modbus unit id */
	uint32_t	point_count[MOGBUS_SPACE_COUNT];	/**< points per mogbus_space_t */
	/** @brief One fault slot of the device. */
	struct
	{
		bool		present;	/**< the fault is configured */
		bool		scheduled;	/**< fires by schedule, not by percent */
		uint32_t	percent;	/**< share of responses, 0-100 */
		uint32_t	delay_ms;	/**< MOGBUS_FAULT_DELAY only */
		uint8_t		exception_code;	/**< MOGBUS_FAULT_EXCEPTION only */
	} faults[MOGBUS_FAULT_KIND_COUNT];	/**< indexed by mogbus_fault_kind_t */
	uint32_t	custom_fc_count;	/**< user-defined function codes, from (function-code ...) */
	/** @brief One user-defined function code of the device. */
	struct
	{
		uint8_t		code;		/**< 1-127 */
		const char*	name;		/**< borrowed; "" when unnamed */
	} custom_fcs[8];
};

/**
 * @brief Fills a snapshot of a device's points and faults.
 * @param dev Device.
 * @param out Receives the snapshot.
 * @return false if @p dev or @p out is NULL.
 */
MOGBUS_API bool		mogbus_device_info(const mogbus_device_t* dev, mogbus_device_info_t* out);

/**
 * @brief Adds a point to a device.
 * @param dev Device.
 * @param point Point description; copied.
 * @param err Buffer for an error message; may be NULL.
 * @param err_size Size of @p err; 0 discards the message.
 * @return false for an invalid point: type does not fit the space, overlapping or out-of-range
 *         address, bad ramp.
 */
MOGBUS_API bool		mogbus_device_add_point(mogbus_device_t* dev, const mogbus_point_t* point,
						char* err, size_t err_size);

/**
 * @brief Reads consecutive raw 16-bit values of a table.
 *
 * Ramps are advanced to the current time first. Bit spaces read as 0 or 1.
 *
 * @param dev Device.
 * @param space Table.
 * @param address First address.
 * @param count Number of addresses.
 * @param out Receives @p count values.
 * @return false, changing nothing, if any address is undefined or out of range.
 */
MOGBUS_API bool		mogbus_device_read(mogbus_device_t* dev, mogbus_space_t space, uint32_t address,
					   uint32_t count, uint16_t* out);
/**
 * @brief Writes consecutive raw 16-bit values, as a client write would.
 *
 * Works on every space, including the read-only ones and points configured with
 * `(access read-only)` (the host plays the physical side). A written ramp becomes static, and matching machine on-write handlers run. Bit spaces accept
 * only 0 or 1.
 *
 * @param dev Device.
 * @param space Table.
 * @param address First address.
 * @param count Number of addresses.
 * @param in @p count values to write.
 * @return false, changing nothing, if any address is undefined or out of range, a value does not
 *         fit, or a point is driven by a machine or claimed with mogbus_device_set_value().
 */
MOGBUS_API bool		mogbus_device_write(mogbus_device_t* dev, mogbus_space_t space, uint32_t address,
					    uint32_t count, const uint16_t* in);

/**
 * @brief Reads the decoded value of the point that starts at @p address.
 *
 * @p address is the point's first address, not an address inside it. Bit points read as
 * 0.0 or 1.0. 64-bit integers above 2^53 are rounded to the nearest double.
 *
 * @param dev Device.
 * @param space Table.
 * @param address First address of the point.
 * @param out Receives the value.
 * @return false if no point starts at @p address, or the point is a string (use
 *         mogbus_device_get_string()).
 */
MOGBUS_API bool		mogbus_device_get_value(mogbus_device_t* dev, mogbus_space_t space,
						uint32_t address, double* out);
/**
 * @brief Sets a point's value from the host side, like a machine output.
 *
 * Works on every space, including read-only ones. The point becomes host-owned: a ramp stops
 * and client writes to it get Modbus exception 02 until the device restarts. Use
 * mogbus_device_write() to change a value the way a client would.
 *
 * @param dev Device.
 * @param space Table.
 * @param address First address of the point.
 * @param value New value; for bit points nonzero means on.
 * @return false if no point starts at @p address, or the point is a string (use
 *         mogbus_device_set_string()).
 */
MOGBUS_API bool		mogbus_device_set_value(mogbus_device_t* dev, mogbus_space_t space,
						uint32_t address, double value);

/**
 * @brief Reads a string point as text, up to its first NUL byte.
 *
 * Reflects client writes. The bytes are returned as stored; clients can write any byte.
 *
 * @param dev Device.
 * @param space MOGBUS_HOLDING or MOGBUS_INPUT.
 * @param address First address of the string point.
 * @param out Receives the NUL-terminated text.
 * @param size Size of @p out; at most 2*length + 1 bytes are needed.
 * @return false if no string point starts at @p address or @p out is too small.
 */
MOGBUS_API bool		mogbus_device_get_string(mogbus_device_t* dev, mogbus_space_t space,
						 uint32_t address, char* out, size_t size);

/**
 * @brief Replaces a string point's text from the host side, NUL padded.
 *
 * Unlike mogbus_device_set_value() it does not make the point host-owned: clients can still
 * write it, and a restart restores the configured text.
 *
 * @param dev Device.
 * @param space MOGBUS_HOLDING or MOGBUS_INPUT.
 * @param address First address of the string point.
 * @param text NUL-terminated text, at most 2*length bytes.
 * @return false if no string point starts at @p address or @p text is too long.
 */
MOGBUS_API bool		mogbus_device_set_string(mogbus_device_t* dev, mogbus_space_t space,
						 uint32_t address, const char* text);

/* ---- Server ---- */

/** @brief Transport a server uses. New modes are appended, so values stay stable. */
enum mogbus_mode_t
{
	MOGBUS_MODE_TCP,		/**< Modbus TCP (MBAP), optionally TLS */
	MOGBUS_MODE_RTU_PTY,		/**< RTU on a new PTY; see mogbus_server_pty_name() */
	MOGBUS_MODE_RTU_DEVICE,		/**< RTU on the serial device `path` */
	MOGBUS_MODE_RTU_TCP,		/**< RTU frames over TCP */
	MOGBUS_MODE_ASCII_PTY,		/**< ASCII on a new PTY; see mogbus_server_pty_name() */
	MOGBUS_MODE_ASCII_DEVICE,	/**< ASCII on the serial device `path` */
	MOGBUS_MODE_ASCII_TCP,		/**< ASCII frames over TCP */
	MOGBUS_MODE_RTU_FD,		/**< RTU on an open byte-stream fd you own */
	MOGBUS_MODE_ASCII_FD,		/**< ASCII on an open byte-stream fd you own */
	MOGBUS_MODE_UDP,		/**< Modbus UDP: one MBAP ADU per datagram */
	MOGBUS_MODE_ASCII_UDP,		/**< Modbus ASCII: one CRLF-terminated frame per datagram */
	MOGBUS_MODE_RTU_UDP,		/**< RTU frames, one per UDP datagram */
};

/** @brief Serial line parity; mark and space need CMSPAR support. */
enum mogbus_parity_t
{
	MOGBUS_PARITY_NONE,
	MOGBUS_PARITY_EVEN,
	MOGBUS_PARITY_ODD,
	MOGBUS_PARITY_MARK,
	MOGBUS_PARITY_SPACE,
};

/** @brief Minimum TLS version of a TLS server. */
enum mogbus_tls_version_t
{
	MOGBUS_TLS_1_2,
	MOGBUS_TLS_1_3,
};

/**
 * @brief Server settings. Start from mogbus_server_options_defaults(); fields a mode does not use
 *        are ignored.
 */
struct mogbus_server_options_t
{
	mogbus_mode_t	mode;			/**< transport */
	const char*	bind_address;		/**< TCP-family and UDP modes; "0.0.0.0" opts in to remote access */
	uint16_t	port;			/**< TCP-family and UDP modes; use 802 by convention with TLS */
	const char*	path;			/**< *_DEVICE modes */
	int		fd;			/**< *_FD modes: yours, never closed by the library */
	uint32_t	seed;			/**< fault dice seed; 0 = fixed default */
	uint32_t	idle_timeout_ms;	/**< TCP-family modes; 0 = never */
	uint32_t	frame_gap_ms;		/**< RTU modes: inter-frame gap override; 0 = auto */
	uint32_t	baud;			/**< RTU timing and serial speed; 0 = unknown */
	bool		strict_t15;		/**< RTU byte streams: discard frames with a t1.5 gap */
	bool		broadcast_execute;	/**< RTU modes: run unit-0 requests (still no reply) */
	uint32_t	data_bits;		/**< serial: 7 or 8 (RTU always uses 8) */
	mogbus_parity_t	parity;			/**< serial */
	uint32_t	stop_bits;		/**< serial: 1 or 2 */
	bool		tls_enabled;		/**< MOGBUS_MODE_TCP only; needs a build with TLS */
	uint32_t	tls_handshake_timeout_ms;	/**< close unfinished handshakes after this */
	const char*	tls_certificate;	/**< server certificate chain, PEM file */
	const char*	tls_private_key;	/**< server private key, PEM file */
	const char*	tls_ca_certificate;	/**< NULL = do not verify clients */
	bool		tls_require_client_certificate;	/**< reject clients without a certificate */
	mogbus_tls_version_t tls_min_version;	/**< minimum TLS version */
	const char*	tls_ciphers_1_2;	/**< NULL = OpenSSL default */
	const char*	tls_ciphersuites_1_3;	/**< NULL = OpenSSL default */
	const char*	http_bind_address;	/**< HTTP /healthz, /readyz, /metrics endpoint; NULL = off */
	uint16_t	http_port;		/**< endpoint port, used when http_bind_address is set */
	bool		http_control;		/**< serve the runtime control API under /api/ (needs the endpoint; no authentication) */
};

/**
 * @brief Returns the defaults of a config file.
 *
 * TCP on 127.0.0.1:1502, no idle timeout, 8N1, TLS off (when enabled: handshake timeout
 * 10000 ms, client certificates required, TLS 1.2 minimum), fd -1.
 * @return Default options.
 */
MOGBUS_API mogbus_server_options_t	mogbus_server_options_defaults(void);

/** @brief Why a server could not be created (see mogbus_server_create()). */
enum mogbus_error_t
{
	MOGBUS_OK,		/**< no error */
	MOGBUS_ERR_INVALID,	/**< bad options, no devices, or a limit reached */
	MOGBUS_ERR_SERIAL,	/**< the serial device or PTY could not be opened */
	MOGBUS_ERR_LISTEN,	/**< the listener could not start (port in use) or TLS setup failed */
};

/**
 * @brief Starts a server for every device the context has now.
 *
 * Devices added later are not served.
 *
 * @param ctx Context with at least one device.
 * @param options Server settings; strings are copied.
 * @param err Buffer for an error message; may be NULL.
 * @param err_size Size of @p err; 0 discards the message.
 * @param status Receives MOGBUS_OK or the reason for the failure; may be NULL.
 * @return The server, or NULL if the options are invalid or the transport cannot start (port in
 *         use, serial device missing, TLS files unreadable, no devices, MOGBUS_MAX_SERVERS reached).
 */
MOGBUS_API mogbus_server_t*	mogbus_server_create(mogbus_t* ctx, const mogbus_server_options_t* options,
						     char* err, size_t err_size, mogbus_error_t* status);

/**
 * @brief Starts a server with the (server ...) section of the last mogbus_load_config().
 * @param ctx Context with a loaded config.
 * @param err Buffer for an error message; may be NULL.
 * @param err_size Size of @p err; 0 discards the message.
 * @param status Receives MOGBUS_OK or the reason for the failure; may be NULL.
 * @return The server, or NULL (no config loaded, no config support, or see mogbus_server_create()).
 */
MOGBUS_API mogbus_server_t*	mogbus_server_create_from_config(mogbus_t* ctx, char* err, size_t err_size,
								 mogbus_error_t* status);

/**
 * @brief Returns the (server ...) section of the last mogbus_load_config() as options.
 * @param ctx Context.
 * @param out Receives the options; strings are borrowed from the context.
 * @return false if no config is loaded or the build has no config support.
 */
MOGBUS_API bool		mogbus_config_server_options(const mogbus_t* ctx, mogbus_server_options_t* out);

/**
 * @brief Validates options exactly as mogbus_server_create() would, without starting anything.
 *
 * Checks the device path, fd, serial line, string lengths and TLS files (through a throwaway TLS
 * context), but opens no listener, serial device or PTY, so a port in use still passes.
 *
 * @param ctx Context.
 * @param options Server settings.
 * @param err Buffer for an error message; may be NULL.
 * @param err_size Size of @p err; 0 discards the message.
 * @return true if mogbus_server_create() would accept the options.
 */
MOGBUS_API bool		mogbus_server_check(const mogbus_t* ctx, const mogbus_server_options_t* options,
					    char* err, size_t err_size);

/**
 * @brief Checks that a server answers: one FC 08 Return Query Data round trip.
 *
 * Frames the request like the mode does and works for the TCP, UDP, RTU-over-TCP/UDP and
 * ASCII-over-TCP/UDP modes. A bind of 0.0.0.0 or :: is reached through loopback. A TLS server
 * is only connected to, since the probe has no client certificate. A fault that drops or delays
 * replies on @p unit_id makes the probe fail. It blocks the calling thread, so it cannot probe a
 * server stepped by the same thread.
 *
 * @param options Settings of the server to probe, e.g. from mogbus_config_server_options().
 * @param unit_id Unit to ask; not 0 (broadcast is never answered).
 * @param timeout_ms Time to wait for the reply.
 * @param err Buffer for an error message; may be NULL.
 * @param err_size Size of @p err; 0 discards the message.
 * @return true for the echo or a Modbus exception reply; false on a timeout, a refused
 *         connection, a malformed reply, a serial or fd mode, or unit 0.
 */
MOGBUS_API bool		mogbus_probe(const mogbus_server_options_t* options, uint8_t unit_id,
				     uint32_t timeout_ms, char* err, size_t err_size);

/**
 * @brief Computes the RTU timing a server would use.
 * @param baud Baud rate; 0 = unknown.
 * @param frame_gap_ms Explicit inter-frame gap; 0 = derive it.
 * @param t35_us Receives the Modbus t3.5 in microseconds (0 when @p baud is 0); may be NULL.
 * @param gap_ms Receives the inter-frame gap in milliseconds: @p frame_gap_ms if nonzero, else
 *        derived from @p baud, else 20; may be NULL.
 */
MOGBUS_API void		mogbus_rtu_timing(uint32_t baud, uint32_t frame_gap_ms,
					  uint32_t* t35_us, uint32_t* gap_ms);

/** @brief Results of mogbus_server_step(). */
enum
{
	MOGBUS_STEP_CONTINUE = 0,	/**< keep stepping */
	MOGBUS_STEP_DONE     = 1,	/**< stopped, or the byte-stream peer went away */
	MOGBUS_STEP_ERROR    = 2,	/**< poll failed */
};

/**
 * @brief Runs one poll iteration: accepts, reads, answers and writes whatever is ready.
 *
 * Call it in a loop, interleaved with your own work.
 *
 * @param server Server.
 * @param max_wait_ms Longest time to block: 0 = do not block, -1 = until the server's next
 *        deadline (at most 1 s).
 * @return A MOGBUS_STEP_* value.
 */
MOGBUS_API int		mogbus_server_step(mogbus_server_t* server, int max_wait_ms);

/**
 * @brief Asks the server to stop; the next step returns MOGBUS_STEP_DONE.
 *
 * Async-signal-safe, and the only call that may come from another thread or a signal handler.
 * @param server Server.
 */
MOGBUS_API void		mogbus_server_stop(mogbus_server_t* server);

/**
 * @brief Makes SIGINT and SIGTERM (Ctrl-C on a console) stop the server held in *slot, and
 *        ignores SIGPIPE.
 *
 * The handler calls mogbus_server_stop() on the server stored in *slot, and does nothing while
 * *slot is NULL, so the host may install it before creating the server and store the server
 * afterwards. Process-wide: the library installs no handlers unless this is called, and the
 * slot must stay valid for the life of the process.
 * @param slot Host variable that holds the server to stop.
 */
MOGBUS_API void		mogbus_stop_on_signals(mogbus_server_t* volatile* slot);

/**
 * @brief Steps until stopped or the byte-stream peer goes away, then closes the transport.
 *
 * Still call mogbus_server_close() afterwards to release the handle.
 * @param server Server.
 * @return 0 after a clean stop, 1 on a poll error.
 */
MOGBUS_API int		mogbus_server_run(mogbus_server_t* server);

/**
 * @brief Closes the transport and releases the handle.
 *
 * Client connections are dropped; fds you passed in are left open.
 * @param server Server; NULL is ignored.
 */
MOGBUS_API void		mogbus_server_close(mogbus_server_t* server);

/**
 * @brief Renders the server's metrics in the Prometheus text format.
 *
 * The same text the HTTP endpoint serves on /metrics, for a host that exports metrics itself.
 * Counters start at zero when the server is created.
 *
 * @param server Server.
 * @param out Buffer for the text.
 * @param size Size of @p out; 32 KiB is enough for 16 devices.
 * @return Length of the NUL-terminated text, or 0 if @p size is too small or @p server is NULL.
 */
MOGBUS_API size_t	mogbus_server_metrics(const mogbus_server_t* server, char* out, size_t size);

/**
 * @brief Returns the PTY slave path of a *_PTY mode server.
 * @param server Server.
 * @return The path, borrowed from the server, or NULL in other modes.
 */
MOGBUS_API const char*	mogbus_server_pty_name(const mogbus_server_t* server);

/* ---- Logging (process-wide) ---- */

/** @brief Log levels; a level drops every line below it. */
enum mogbus_log_level_t
{
	MOGBUS_LOG_DEBUG,
	MOGBUS_LOG_INFO,
	MOGBUS_LOG_WARN,
	MOGBUS_LOG_OFF,		/**< no logging */
};

/**
 * @brief Log sink, see mogbus_set_log_sink().
 * @param user The pointer given to mogbus_set_log_sink().
 * @param level Level of the line.
 * @param message The line, without a trailing newline; valid only during the call.
 */
/** @brief Log line format. */
enum mogbus_log_format_t
{
	MOGBUS_LOG_TEXT,	/**< timestamp, level and message (default) */
	MOGBUS_LOG_JSON,	/**< one JSON object per line, with event fields for requests */
};

/** @brief Receives one log line; `user` is the pointer given at registration. */
typedef void (*mogbus_log_fn)(void* user, mogbus_log_level_t level, const char* message);

/**
 * @brief Sends log lines to a function instead of stderr.
 *
 * Process-wide. Lines are truncated to 1023 bytes and delivered synchronously on the thread that
 * logs.
 *
 * @param fn Sink; NULL restores the default (timestamped INFO/WARN lines on stderr).
 * @param user Passed to @p fn unchanged.
 */
MOGBUS_API void		mogbus_set_log_sink(mogbus_log_fn fn, void* user);

/**
 * @brief Sets the process-wide log level; lines below it are dropped.
 * @param level New level; the default is MOGBUS_LOG_INFO.
 */
MOGBUS_API void		mogbus_set_log_level(mogbus_log_level_t level);

/**
 * @brief Sets the process-wide log line format.
 *
 * In MOGBUS_LOG_JSON each line is an object with `ts`, `level` and `msg`; request lines (DEBUG)
 * add `event`, `transport`, `peer`, `unit`, `fc`, `result`, `exception`, `latency_ms` and
 * `faults`. A log sink receives the whole JSON object as its message.
 * @param format New format; the default is MOGBUS_LOG_TEXT.
 */
MOGBUS_API void		mogbus_set_log_format(mogbus_log_format_t format);

#ifdef __cplusplus
}
#endif

#endif
