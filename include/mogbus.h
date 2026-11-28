/**
 * @file
 * @brief Public C API of the mogbus library: a Modbus device simulator to embed in a program.
 *
 * This is the only header a library user needs. It uses only <stdint.h>,
 * <stdbool.h> and <stddef.h> types and every symbol starts with mogbus_ or
 * MOGBUS_. Link with libmogbus.so or libmogbus.a (see docs/user/library/library-start.md).
 *
 * Model: a mogbus_t context owns the devices and servers you create. Devices
 * come from a config file (mogbus_load_config) or are built in code
 * (mogbus_add_device + mogbus_device_add_point). A server exposes the devices assigned to it (by
 * default all devices of the context) over one transport (TCP, RTU, ASCII, ...), and the host drives
 * it with mogbus_server_step() from its own loop, or hands the thread over with
 * mogbus_server_run().
 *
 * Rules:
 *  - Thread-safe per context. Every call that takes a context, one of its devices or one of
 *    its servers holds one recursive lock of that context for the call, so any thread may
 *    call them at any time, including while another thread runs mogbus_server_run() or
 *    steps the server. The step releases the lock while it waits for I/O and holds it while
 *    it answers requests. Hooks and callbacks run with the lock held on the stepping thread
 *    and may call back into the context. The exceptions: mogbus_server_stop(),
 *    mogbus_stop() and the signal helpers take no lock (async-signal-safe, any thread; a
 *    running step notices within one second); a client (mogbus_client_t) is used by one thread
 *    at a time; mogbus_destroy() and mogbus_server_close() must not race with any other call
 *    on the same context or server; logging settings are process-wide and belong to
 *    start-up; the message of mogbus_last_error() is shared by the threads of a context.
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
 *  - The library never installs signal handlers. Its sockets, TLS ones included, are written
 *    without raising SIGPIPE when the peer vanishes.
 *  - A fallible call reports failure one way only: it returns a mogbus_result_t, MOGBUS_OK on
 *    success. A constructor hands the object back through its last parameter, which is set to NULL
 *    on failure (`mogbus_create(&ctx)`). The text of the failure is kept by the context:
 *    mogbus_last_error(). Only mogbus_create(), mogbus_probe() and mogbus_client_open(), which have
 *    no owner yet, keep their text per thread (mogbus_last_error(NULL)); a client keeps the text
 *    of its own calls (mogbus_client_last_error()). Successful calls leave the last error alone.
 *    Accessors that cannot fail with valid handles (counts, `*_at` lookups, defaults) return
 *    their value directly.
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
typedef enum mogbus_clock_field_t mogbus_clock_field_t;
typedef struct mogbus_point_t mogbus_point_t;
typedef enum mogbus_fault_kind_t mogbus_fault_kind_t;
typedef struct mogbus_device_info_t mogbus_device_info_t;
typedef enum mogbus_access_kind_t mogbus_access_kind_t;
typedef enum mogbus_access_target_t mogbus_access_target_t;
typedef struct mogbus_access_t mogbus_access_t;
typedef enum mogbus_mode_t mogbus_mode_t;
typedef enum mogbus_parity_t mogbus_parity_t;
typedef enum mogbus_tls_version_t mogbus_tls_version_t;
typedef struct mogbus_server_options_t mogbus_server_options_t;
typedef struct mogbus_client_options_t mogbus_client_options_t;
typedef struct mogbus_client_file_record_t mogbus_client_file_record_t;
typedef enum mogbus_frame_direction_t mogbus_frame_direction_t;
typedef struct mogbus_frame_t mogbus_frame_t;
typedef struct mogbus_poll_fd_t mogbus_poll_fd_t;
typedef enum mogbus_close_reason_t mogbus_close_reason_t;
typedef struct mogbus_connection_t mogbus_connection_t;
typedef enum mogbus_result_t mogbus_result_t;
typedef enum mogbus_log_level_t mogbus_log_level_t;
typedef enum mogbus_log_format_t mogbus_log_format_t;
typedef enum mogbus_framing_t mogbus_framing_t;
typedef enum mogbus_direction_t mogbus_direction_t;
/** @brief A context: owns devices, servers and their memory. */
typedef struct mogbus_t		mogbus_t;
/** @brief A simulated device (one unit id), owned by its context. */
typedef struct mogbus_device_t	mogbus_device_t;
/** @brief A running server: one transport serving the devices of a context. */
typedef struct mogbus_server_t	mogbus_server_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Windows: the library build exports (MOGBUS_BUILD); DLL users define MOGBUS_DLL to import; a static
 * library user defines neither. */
#if defined(_WIN32) && defined(MOGBUS_BUILD)
#define MOGBUS_API __declspec(dllexport)
#elif defined(_WIN32) && defined(MOGBUS_DLL)
#define MOGBUS_API __declspec(dllimport)
#elif defined(_WIN32)
#define MOGBUS_API
#else
#define MOGBUS_API __attribute__((visibility("default")))
#endif

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
#define MOGBUS_MAX_POINTS	8192	/**< Maximum number of points of one device. */

/* ---- Context ---- */

/**
 * @brief Creates an empty context.
 * @param out Receives the context, or NULL on failure.
 * @return MOGBUS_OK, or MOGBUS_ERR_NOMEM if its memory cannot be mapped (the message is
 *         mogbus_last_error(NULL)).
 */
MOGBUS_API mogbus_result_t	mogbus_create(mogbus_t** out);

/**
 * @brief Closes every server of the context and releases all its memory, devices included.
 * @param ctx Context to destroy; NULL is ignored.
 */
MOGBUS_API void		mogbus_destroy(mogbus_t* ctx);

/**
 * @brief Status of a fallible call (see mogbus_last_error()).
 *
 * The `mogbus` binary maps MOGBUS_ERR_SERIAL to exit status 1 and MOGBUS_ERR_LISTEN to 2. A TLS
 * setup failure is also logged at WARN, like the transports log their own start-up errors.
 */
enum mogbus_result_t
{
	MOGBUS_OK,		/**< no error */
	MOGBUS_ERR_INVALID,	/**< bad argument or options, no devices, or a limit reached */
	MOGBUS_ERR_SERIAL,	/**< the serial device or PTY could not be opened */
	MOGBUS_ERR_LISTEN,	/**< the listener could not start (port in use) or TLS setup failed */
	MOGBUS_ERR_CONFIG,	/**< a config file could not be read or is invalid */
	MOGBUS_ERR_UNSUPPORTED,	/**< the library was built without config support */
	MOGBUS_ERR_NOMEM,	/**< the context arena is exhausted */
	MOGBUS_ERR_PROBE,	/**< mogbus_probe(): connection, send or reply failure */
	MOGBUS_ERR_IO,		/**< client: connect, send or receive failed, or the server closed the connection */
	MOGBUS_ERR_TIMEOUT,	/**< client: no reply within the timeout (after the retries) */
	MOGBUS_ERR_EXCEPTION,	/**< client: the server answered with a Modbus exception, see mogbus_client_last_exception() */
	MOGBUS_ERR_FRAME,	/**< client: malformed reply, or one that does not answer the request; mogbus_decode_frame(): the frame is malformed (its text is still written) */
};

/**
 * @brief Message of the most recent failed call on a context.
 *
 * Every failing call that takes the context (or a device of it) overwrites it; a successful call
 * leaves it unchanged. The pointer stays valid until the next failing call on @p ctx or
 * mogbus_destroy(). The failing call returns the code; this gives the text.
 *
 * @param ctx Context; NULL gives the message of the last failed mogbus_probe() or
 *        mogbus_client_open() or mogbus_create() of the calling thread.
 * @return The message, "" if nothing failed yet.
 */
MOGBUS_API const char*	mogbus_last_error(const mogbus_t* ctx);

/**
 * @brief Loads a config file into the context.
 *
 * Adds the file's devices and remembers its (server ...) section for
 * mogbus_server_create_from_config(). On failure (bad file, too many devices, a unit id that
 * already exists) the context is unchanged. A later call replaces the remembered server section.
 * See docs/user/config/format.md.
 *
 * @param ctx Context.
 * @param path Config file path.
 * @return MOGBUS_OK, MOGBUS_ERR_CONFIG for a bad file, too many devices or a unit id already in
 *         use, MOGBUS_ERR_NOMEM, or MOGBUS_ERR_UNSUPPORTED in a build without config support. The
 *         message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_load_config(mogbus_t* ctx, const char* path);

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
	MOGBUS_POINT_CLOCK,	/**< shows `clock_field` of the wall clock (integer registers only) */
	MOGBUS_POINT_MODEL,	/**< follows a config model (sine, lag, noise); reported by mogbus_device_point_at() only */
	MOGBUS_POINT_MACHINE,	/**< driven by a config machine; reported by mogbus_device_point_at() only */
};

/** @brief Which wall-clock value a MOGBUS_POINT_CLOCK point shows. */
enum mogbus_clock_field_t
{
	MOGBUS_CLOCK_YEAR,
	MOGBUS_CLOCK_MONTH,	/**< 1-12 */
	MOGBUS_CLOCK_DAY,	/**< 1-31 */
	MOGBUS_CLOCK_WEEKDAY,	/**< ISO: Monday 1 ... Sunday 7 */
	MOGBUS_CLOCK_HOUR,
	MOGBUS_CLOCK_MINUTE,
	MOGBUS_CLOCK_SECOND,
	MOGBUS_CLOCK_UNIX,	/**< seconds since the epoch, always UTC; needs a 32-bit or wider type */
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
 * overlap and must fit below 65536. Fields were appended over time (`byte_order`, `length`,
 * `text`, `read_only`, `clock_*`), so code built against an older header keeps working when it
 * zero-initialises the struct.
 *
 * Register layout of a 32-bit value 0x0A0B0C0D (64-bit types extend the same rule), by
 * `word_order` and `byte_order`:
 *
 * | word_order | byte_order | registers (low address first) | name |
 * |---|---|---|---|
 * | MOGBUS_WORDS_HIGH_FIRST | MOGBUS_BYTES_BIG | 0x0A0B 0x0C0D | ABCD |
 * | MOGBUS_WORDS_LOW_FIRST | MOGBUS_BYTES_BIG | 0x0C0D 0x0A0B | CDAB |
 * | MOGBUS_WORDS_HIGH_FIRST | MOGBUS_BYTES_LITTLE | 0x0B0A 0x0D0C | BADC |
 * | MOGBUS_WORDS_LOW_FIRST | MOGBUS_BYTES_LITTLE | 0x0D0C 0x0B0A | DCBA |
 *
 * A string point has `length` registers of two ASCII bytes each, holding or input table only;
 * read and replace it with mogbus_device_get_string() and mogbus_device_set_string(). `read_only`
 * makes every Modbus write that touches the point fail with exception 02, like
 * `(access read-only)` in a config.
 */
struct mogbus_point_t
{
	mogbus_space_t		space;		/**< table the point lives in */
	uint32_t		address;	/**< first address, below 65536 */
	mogbus_value_type_t	type;		/**< register encoding */
	mogbus_word_order_t	word_order;	/**< 32- and 64-bit types only */
	mogbus_point_kind_t	kind;		/**< static, ramp or clock */
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
	mogbus_clock_field_t	clock_field;	/**< MOGBUS_POINT_CLOCK: the wall-clock value shown */
	bool			clock_utc;	/**< MOGBUS_POINT_CLOCK: UTC instead of local time */
	const char*		note;		/**< short human note shown next to the point in the register
						     view and the web UI; at most 127 bytes, copied; NULL or
						     empty means none; mogbus_device_point_at() returns it */
};

/**
 * @brief Adds an empty device to the context.
 *
 * Points support static values, ramps and wall-clock fields. Machines, faults and user-defined
 * function codes are only available through config files (mogbus_load_config()); code-built and
 * loaded devices can be mixed in one context, which holds up to MOGBUS_MAX_DEVICES devices.
 *
 * @param ctx Context.
 * @param unit_id Modbus unit id. Two devices may share one only when they are assigned to
 *        different servers (mogbus_device_add_server()); mogbus_server_create() and
 *        mogbus_device_add_server() fail when two devices of one server have the same id.
 * @param name Device name, copied, at most 127 bytes; NULL means "unnamed".
 * @param out Receives the device, or NULL on failure.
 * @return MOGBUS_OK, MOGBUS_ERR_INVALID for a too long name or when MOGBUS_MAX_DEVICES is
 *         reached, or MOGBUS_ERR_NOMEM. The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_add_device(mogbus_t* ctx, uint8_t unit_id, const char* name,
						  mogbus_device_t** out);

/**
 * @brief Finds a device by unit id, including devices loaded from config.
 * @param ctx Context.
 * @param unit_id Unit id to look up.
 * @return The first device with that unit id, or NULL (several may share it on different servers).
 */
MOGBUS_API mogbus_device_t*	mogbus_find_device(mogbus_t* ctx, uint8_t unit_id);

/**
 * @brief Assigns the device to the server with the given id.
 *
 * A server created with that `id` serves the device; the device then no longer belongs to the
 * servers without id. Call it again to serve the device from several servers. Only servers
 * created afterwards are affected. Two devices with the same unit id cannot share a server.
 *
 * @param dev Device.
 * @param id Server id, 1 to 31 bytes; copied. At most 8 servers per device.
 * @return MOGBUS_OK (also when already assigned), or MOGBUS_ERR_INVALID for an empty or too long
 *         id, too many servers or a unit id already used by a device of that server; the
 *         message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_device_add_server(mogbus_device_t* dev, const char* id);

/**
 * @brief Returns the id of a server the device is assigned to.
 * @param dev Device.
 * @param index Position in the order of mogbus_device_add_server() calls (or of the config's
 *        `(servers ...)`; a config with one named server assigns it to every device that lists none).
 * @return The id, borrowed from the device, or NULL when @p index is out of range, so index 0 is
 *         NULL for a device served by the servers without id.
 */
MOGBUS_API const char*	mogbus_device_server_at(const mogbus_device_t* dev, size_t index);

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

/** @brief Fault kinds a config file can attach to a device (see docs/user/config/faults/fault.md). */
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
 * MOGBUS_FAULT_EXCEPTION. Faults come only from a config loaded with mogbus_load_config(); the C API
 * cannot add them. User-defined function codes come from the config or from
 * mogbus_device_add_function_code(); the latter are listed with an empty name.
 */
struct mogbus_device_info_t
{
	const char*	name;			/**< borrowed from the device */
	uint8_t		unit_id;		/**< Modbus unit id */
	bool		any_unit;		/**< also answers every unit id no other device has */
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
	uint32_t	custom_fc_count;	/**< user-defined function codes, from (function-code ...) or mogbus_device_add_function_code() */
	/** @brief One user-defined function code of the device. */
	struct
	{
		uint8_t		code;		/**< 1-127 */
		const char*	name;		/**< borrowed; "" when unnamed */
	} custom_fcs[8];
};

/**
 * @brief Makes a device also answer every unit id that no device of the context has.
 *
 * Exact unit ids still win. RTU and ASCII never answer the broadcast address 0 or the reserved
 * addresses 248-255 this way; on Modbus TCP/UDP unit 0 and 255 are ordinary addresses. Only one
 * device of a context can have it (the `(any-unit on)` config key does the same).
 *
 * @param dev Device.
 * @param on true to enable, false to disable.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID if @p dev is NULL or another device of the context
 *         already has it. The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_device_set_any_unit(mogbus_device_t* dev, bool on);

/**
 * @brief Fills a snapshot of a device's points and faults.
 * @param dev Device.
 * @param out Receives the snapshot.
 * @return false if @p dev or @p out is NULL.
 */
MOGBUS_API bool		mogbus_device_info(const mogbus_device_t* dev, mogbus_device_info_t* out);

/**
 * @brief Describes the point at an index of the device's points, in definition order.
 *
 * The index runs from 0 to the sum of mogbus_device_info() `point_count` minus one. Only the
 * layout fields are filled (`space`, `address`, `type`, `word_order`, `byte_order`, `length`,
 * `read_only`, `clock_field`, `clock_utc`); `value`, `ramp` and `text` are zero, and read the
 * current value with mogbus_device_get_value() or mogbus_device_get_string(). `kind` also takes
 * MOGBUS_POINT_MODEL and MOGBUS_POINT_MACHINE for points a config made that way.
 *
 * @param dev Device.
 * @param index Position of the point.
 * @param out Receives the description.
 * @return false if @p dev or @p out is NULL or @p index is past the last point.
 */
MOGBUS_API bool		mogbus_device_point_at(const mogbus_device_t* dev, uint32_t index,
					       mogbus_point_t* out);

/**
 * @brief Adds a point to a device.
 * @param dev Device.
 * @param point Point description; copied.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID for an invalid point: type does not fit the space,
 *         overlapping or out-of-range address, bad ramp. The message is in mogbus_last_error()
 *         of the device's context.
 */
MOGBUS_API mogbus_result_t	mogbus_device_add_point(mogbus_device_t* dev, const mogbus_point_t* point);

/**
 * @brief Reads consecutive raw 16-bit values of a table.
 *
 * Ramps are advanced to the current time first. Bit spaces read as 0 or 1. Host side: it never
 * calls the access hook.
 *
 * @param dev Device.
 * @param space Table.
 * @param address First address.
 * @param count Number of addresses.
 * @param out Receives @p count values.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID, changing nothing, if any address is undefined or out
 *         of range. The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_device_read(mogbus_device_t* dev, mogbus_space_t space, uint32_t address,
						   uint32_t count, uint16_t* out);
/**
 * @brief Writes consecutive raw 16-bit values, as a client write would.
 *
 * Works on every space, including the read-only ones and points configured with
 * `(access read-only)` or `read_only` (the host plays the physical side). A written ramp becomes
 * static, and matching machine on-write handlers run. Bit spaces accept only 0 or 1. It works on
 * raw register addresses (a float32 is two registers), unlike mogbus_device_set_value(), which
 * addresses a point. It never calls the access hook.
 *
 * @param dev Device.
 * @param space Table.
 * @param address First address.
 * @param count Number of addresses.
 * @param in @p count values to write.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID, changing nothing, if any address is undefined or out
 *         of range, a value does not fit, or a point is driven by a machine or claimed with
 *         mogbus_device_set_value(). The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_device_write(mogbus_device_t* dev, mogbus_space_t space, uint32_t address,
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
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID if no point starts at @p address, or the point is a
 *         string (use mogbus_device_get_string()). The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_device_get_value(mogbus_device_t* dev, mogbus_space_t space,
							uint32_t address, double* out);
/**
 * @brief Sets a point's value from the host side, like a machine output.
 *
 * Works on every space, including read-only ones. The point becomes host-owned: a ramp stops
 * and client writes to it get Modbus exception 02 until the device restarts. Use
 * mogbus_device_write() to change a value the way a client would. It never calls the access hook.
 *
 * @param dev Device.
 * @param space Table.
 * @param address First address of the point.
 * @param value New value; for bit points nonzero means on.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID if no point starts at @p address, or the point is a
 *         string (use mogbus_device_set_string()). The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_device_set_value(mogbus_device_t* dev, mogbus_space_t space,
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
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID if no string point starts at @p address or @p out is
 *         too small. The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_device_get_string(mogbus_device_t* dev, mogbus_space_t space,
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
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID if no string point starts at @p address or @p text is
 *         too long. The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_device_set_string(mogbus_device_t* dev, mogbus_space_t space,
							 uint32_t address, const char* text);

/** @brief Kind of Modbus client access an access hook is asked about. */
enum mogbus_access_kind_t
{
	MOGBUS_ACCESS_READ,	/**< a client is about to read a range */
	MOGBUS_ACCESS_WRITE,	/**< a client is about to write a range */
};

/** @brief What a Modbus client access touches. */
enum mogbus_access_target_t
{
	MOGBUS_TARGET_TABLE,		/**< a range of one of the four tables (the original behaviour) */
	MOGBUS_TARGET_FILE_RECORD,	/**< one sub-request of FC 14 (read) or 15 (write) */
	MOGBUS_TARGET_FIFO,		/**< an FC 18 FIFO queue read */
	MOGBUS_TARGET_CANOPEN,		/**< an FC 2B/0D CANopen object read or write */
};

/**
 * @brief A Modbus client access, as seen by an access hook.
 *
 * `target` says which fields are meaningful. The fields from `target` on are only set for the
 * non-table targets (zero for tables); they are appended so older code that ignores them still works.
 */
struct mogbus_access_t
{
	mogbus_access_kind_t kind;	/**< read or write */
	mogbus_space_t	space;		/**< TABLE: table touched */
	uint32_t	address;	/**< TABLE: first address; FILE_RECORD: first record; FIFO: pointer address; CANOPEN: first byte offset in the object */
	uint32_t	count;		/**< TABLE: registers or bits; FILE_RECORD: records; FIFO: 0; CANOPEN: bytes */
	const uint16_t*	values;		/**< TABLE and FILE_RECORD writes: the raw registers (bits as 0/1) about to be stored; reads: NULL */
	uint8_t		function_code;	/**< function code of the request (0x03, 0x10, 0x14, 0x2B, ...) */
	mogbus_access_target_t target;	/**< what is touched; MOGBUS_TARGET_TABLE (0) for the four tables */
	uint16_t	file;		/**< FILE_RECORD: file number */
	uint8_t		node;		/**< CANOPEN: node id */
	uint8_t		subindex;	/**< CANOPEN: object subindex */
	uint16_t	index;		/**< CANOPEN: object index */
	const uint8_t*	data;		/**< CANOPEN writes: the bytes about to be stored; reads: NULL */
	uint32_t	data_len;	/**< CANOPEN writes: bytes in `data` (equal to `count`); reads: 0 */
};

/**
 * @brief Access hook: runs on every Modbus client read or write of a device.
 *
 * It runs on the thread that steps the server and may call the other `mogbus_device_*` functions
 * on the same device (for example mogbus_device_set_value() to refresh a value before a read).
 * The pointers in the access (`values`, `data`) are valid only during the call. Code that handles
 * only tables must check `target == MOGBUS_TARGET_TABLE`, because the other targets also reach a
 * hook that was written before they existed.
 *
 * @param user Pointer given to mogbus_device_set_access_hook().
 * @param dev Device being accessed.
 * @param access The access.
 * @return 0 to let the request go on, or a Modbus exception code (1-255) to answer instead; a
 *         vetoed write changes nothing.
 */
typedef uint8_t (*mogbus_access_fn)(void* user, mogbus_device_t* dev, const mogbus_access_t* access);

/**
 * @brief Installs (or, with @p fn NULL, removes) the access hook of a device.
 *
 * Only Modbus client requests call it, after the range is known to be defined (reads) or
 * defined and writable (writes), never host calls (mogbus_device_write(), mogbus_device_set_value(),
 * the HTTP control API or machines). An undefined address, a read-only point or a machine-driven
 * point answers exception 02 without calling the hook. Covered (hex codes): FC 01-04 (before the table is read), 05, 06, 0F, 10,
 * 16 (the masked result) and 17 (write hook first, then the read hook once the write is applied),
 * the `registers` action of user-defined function codes, file records (FC 14 read hook once per
 * sub-request before the response is built; FC 15 write hook for every sub-request before anything is
 * written), FIFO (FC 18, before the queue is read) and CANopen objects (FC 2B/0D, before the object is read
 * or written; a veto is answered as the extended failure `CE 0x08000020`, whatever the nonzero
 * return, because the CANopen response has no place for a Modbus exception code). Not covered:
 * diagnostics and identification.
 *
 * Details per request: the hook sees the raw values about to be stored, so FC 16 reports the masked
 * result. FC 17 runs the write hook first and the read hook once the write is applied (an exception
 * from the read hook leaves the write applied). FC 14 runs the read hook once per sub-request, in
 * order, after the whole request validated, and a nonzero return skips the later sub-requests;
 * FC 15 runs the write hook for every sub-request before anything is written. FC 18 runs it after
 * the FIFO address is known to exist, so the hook can refresh the queue with mogbus_device_set_fifo().
 * A vetoed write changes nothing, so writes stay all-or-nothing. A returned exception counts like
 * any other (diagnostic counter, metrics). With `broadcast-execute` on, the hook runs once per
 * device. Faults still act on the response.
 *
 * A device has one hook; a new one replaces it. The hook is not part of a config and does not
 * survive mogbus_destroy(). It runs on the thread that steps the server, so it must not block.
 *
 * @param dev Device.
 * @param fn Hook, or NULL to remove it.
 * @param user Passed back to @p fn.
 * @return MOGBUS_OK, MOGBUS_ERR_INVALID if @p dev is NULL, or MOGBUS_ERR_NOMEM.
 */
MOGBUS_API mogbus_result_t	mogbus_device_set_access_hook(mogbus_device_t* dev, mogbus_access_fn fn, void* user);

/**
 * @brief Post-write hook: runs after a Modbus client write was applied.
 *
 * It runs on the thread that steps the server and may call the other `mogbus_device_*` functions
 * on the same device (they take the host path, so they never call the hooks again). The pointers
 * in the access are valid only during the call.
 *
 * @param user Pointer given to mogbus_device_set_written_hook().
 * @param dev Device that was written.
 * @param access The write: `kind` is MOGBUS_ACCESS_WRITE, `values` the registers (bits as 0/1) that
 *        were stored (the masked result for FC 16), `function_code`, `target` and the range as for
 *        the access hook (MOGBUS_TARGET_TABLE or MOGBUS_TARGET_FILE_RECORD).
 */
typedef void (*mogbus_written_fn)(void* user, mogbus_device_t* dev, const mogbus_access_t* access);

/**
 * @brief Installs (or, with @p fn NULL, removes) the post-write hook of a device.
 *
 * pymodbus calls `setValues` after a write; this is the same notification, so a host can react to
 * a client write (a mode change, a command register) instead of polling for it. It is called once
 * per written range, after the write is applied, for FC 05, 06, 0F, 10, 16 (the masked result), 17
 * (the write half, before the read half is answered) and, once per sub-request, for FC 15 file
 * records (MOGBUS_TARGET_FILE_RECORD). With `broadcast-execute` on it runs once per device.
 *
 * It is not called for a write the access hook vetoed, for a request answered with an exception,
 * or for host writes (mogbus_device_write(), mogbus_device_set_value(), the HTTP control API,
 * machines). Faults do not change request processing, so it is also called when the response is
 * then dropped, delayed or corrupted; a `restart` fault resets the device after the response is
 * built, so the hook has already run. FC 15 sub-requests are applied in order and the hook of an
 * earlier one runs before the later ones are written.
 *
 * A device has one hook; a new one replaces it. The hook is not part of a config and does not
 * survive mogbus_destroy(). It must not block.
 *
 * @param dev Device.
 * @param fn Hook, or NULL to remove it.
 * @param user Passed back to @p fn.
 * @return MOGBUS_OK, MOGBUS_ERR_INVALID if @p dev is NULL, or MOGBUS_ERR_NOMEM.
 */
MOGBUS_API mogbus_result_t	mogbus_device_set_written_hook(mogbus_device_t* dev, mogbus_written_fn fn, void* user);

/**
 * @name User-defined function codes with a callback
 * @{
 */

/** @brief mogbus_function_fn result: send no response (the request still counts and is logged). */
enum { MOGBUS_FUNCTION_NO_RESPONSE = -1 };

/** @brief Largest reply data of a mogbus_function_fn: the 253-byte PDU less the function code. */
enum { MOGBUS_FUNCTION_REPLY_MAX = 252 };

/**
 * @brief Callback that answers a user-defined function code (see mogbus_device_add_function_code()).
 *
 * It runs on the thread that steps the server and may call the other `mogbus_device_*` functions on
 * the same device; it must not block. `data` is valid only during the call.
 *
 * @param user Pointer given to mogbus_device_add_function_code().
 * @param dev Device answering.
 * @param unit_id Unit id of the request (the device's own, or the one an any-unit device
 *        received; 0 for a broadcast executed on every device).
 * @param function_code The function code (1-127).
 * @param data Request data: the bytes after the function code.
 * @param len Bytes in @p data.
 * @param reply Buffer for the reply data, the bytes after the function code.
 * @param reply_cap Size of @p reply, MOGBUS_FUNCTION_REPLY_MAX.
 * @param reply_len Set to the bytes written to @p reply when returning 0.
 * @return 0 to answer with `reply[0..*reply_len)`; 1-255 to answer that Modbus exception;
 *         MOGBUS_FUNCTION_NO_RESPONSE for silence. Any other value, or `*reply_len` above
 *         @p reply_cap, is answered as exception 04 (slave device failure).
 */
typedef int (*mogbus_function_fn)(void* user, mogbus_device_t* dev, uint8_t unit_id,
				  uint8_t function_code, const uint8_t* data, size_t len,
				  uint8_t* reply, size_t reply_cap, size_t* reply_len);

/**
 * @brief Defines a user-defined function code answered by a callback.
 *
 * Works like `(function-code CODE ...)` in a config, with the callback in place of the cases: the
 * request goes through the same unit routing, faults, listen-only, FC 08 counters, metrics, access
 * log and (with `broadcast-execute`) broadcast handling, where the callback runs once per device
 * and its reply is dropped. A callback's reply is limited to MOGBUS_FUNCTION_REPLY_MAX bytes.
 * Codes share the limit of 8 per device with the config ones. The code is listed by
 * mogbus_device_info() with an empty name. It is not part of a config, cannot be removed and does not
 * survive mogbus_destroy().
 *
 * @param dev Device.
 * @param code Function code, 1-127 and not one mogbus implements.
 * @param fn Callback; must not be NULL.
 * @param user Passed back to @p fn.
 * @return MOGBUS_OK, MOGBUS_ERR_INVALID if @p dev or @p fn is NULL, the code is out of range,
 *         built in or already defined on the device (by config or by this function) or the device
 *         already has 8 codes, or MOGBUS_ERR_NOMEM. The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_device_add_function_code(mogbus_device_t* dev, uint8_t code,
								mogbus_function_fn fn, void* user);

/** @} */

/**
 * @brief Adds file records to a device, as `(file N (record R (values ...)))` does in a config.
 *
 * Clients read them with FC 14 and write them with FC 15. Records of one file must not overlap.
 * The host reads and changes them with mogbus_device_read_file_records() and
 * mogbus_device_write_file_records(); those never call the access hook. Values are 16-bit registers.
 *
 * @param dev Device.
 * @param file File number, 1-65535.
 * @param record First record number; @p record + @p count - 1 must be at most 9999.
 * @param count Number of records, 1-126.
 * @param values @p count initial register values; copied.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID for a NULL argument or an invalid range, an overlap,
 *         too many files (16) or records per file (64), or out of memory. The message is in
 *         mogbus_last_error() of the device's context.
 */
MOGBUS_API mogbus_result_t	mogbus_device_add_file_record(mogbus_device_t* dev, uint16_t file, uint16_t record,
							      uint16_t count, const uint16_t* values);

/**
 * @brief Reads @p count consecutive file records starting at @p record (host side, no hook).
 * @param dev Device.
 * @param file File number.
 * @param record First record number.
 * @param count Number of records.
 * @param out Receives @p count values.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID if the file or any record is not defined, @p count is 0
 *         or @p out is NULL. The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_device_read_file_records(mogbus_device_t* dev, uint16_t file, uint16_t record,
								uint16_t count, uint16_t* out);

/**
 * @brief Writes @p count consecutive file records starting at @p record (host side, no hook).
 * @param dev Device.
 * @param file File number.
 * @param record First record number.
 * @param count Number of records.
 * @param values @p count values to store.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID if the file or any record is not defined, @p count is 0
 *         or @p values is NULL; nothing is written then. The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_device_write_file_records(mogbus_device_t* dev, uint16_t file, uint16_t record,
								 uint16_t count, const uint16_t* values);

/**
 * @brief Adds a FIFO queue that clients read with FC 18 at @p address.
 *
 * Storage always has capacity for 31 values (the Modbus maximum), so mogbus_device_set_fifo() can
 * grow the queue later.
 * @param dev Device.
 * @param address FIFO pointer address.
 * @param count Initial number of values, 0-31.
 * @param values @p count values; copied; may be NULL when @p count is 0.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID for a duplicate address, too many values or queues
 *         (16), or out of memory. The message is in mogbus_last_error() of the device's context.
 */
MOGBUS_API mogbus_result_t	mogbus_device_add_fifo(mogbus_device_t* dev, uint16_t address, uint16_t count,
						       const uint16_t* values);

/**
 * @brief Replaces the contents of the FIFO queue at @p address (host side, no hook).
 *
 * Typically called from an access hook to refresh the queue just before a client reads it.
 *
 * @param dev Device.
 * @param address FIFO pointer address of an existing queue.
 * @param count New number of values, 0-31.
 * @param values @p count values; copied; may be NULL when @p count is 0.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID if no queue is defined at @p address or @p count is
 *         above 31. The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_device_set_fifo(mogbus_device_t* dev, uint16_t address, uint16_t count,
						       const uint16_t* values);

/**
 * @brief Adds a CANopen object served by FC 2B/0D, as `(canopen-object ...)` does in a config.
 * @param dev Device.
 * @param node Node id, 1-127.
 * @param index Object index.
 * @param subindex Object subindex.
 * @param length Object size in bytes, 1-238; fixed for the object's lifetime.
 * @param data @p length initial bytes; copied.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID for an invalid node or length, a duplicate (node,
 *         index, subindex), too many objects (64) or a NULL argument. The message is in
 *         mogbus_last_error() of the device's context.
 */
MOGBUS_API mogbus_result_t	mogbus_device_add_canopen_object(mogbus_device_t* dev, uint8_t node, uint16_t index,
								 uint8_t subindex, uint16_t length, const uint8_t* data);

/**
 * @brief Reads @p count bytes at @p offset of a CANopen object (host side, no hook).
 * @param dev Device.
 * @param node Node id.
 * @param index Object index.
 * @param subindex Object subindex.
 * @param offset First byte.
 * @param count Number of bytes.
 * @param out Receives @p count bytes.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID if the object does not exist or the range is outside
 *         its length. The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_device_read_canopen(mogbus_device_t* dev, uint8_t node, uint16_t index,
							   uint8_t subindex, uint16_t offset, uint16_t count,
							   uint8_t* out);

/**
 * @brief Writes @p count bytes at @p offset of a CANopen object (host side, no hook).
 *
 * The object keeps its length, as for a client write.
 *
 * @param dev Device.
 * @param node Node id.
 * @param index Object index.
 * @param subindex Object subindex.
 * @param offset First byte.
 * @param count Number of bytes.
 * @param data @p count bytes to store.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID if the object does not exist or the range is outside
 *         its length. The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_device_write_canopen(mogbus_device_t* dev, uint8_t node, uint16_t index,
							    uint8_t subindex, uint16_t offset, uint16_t count,
							    const uint8_t* data);

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
 *
 * Fields by mode: TCP uses bind_address, port, idle_timeout_ms and the tls_* fields; RTU_TCP and
 * ASCII_TCP use bind_address, port and idle_timeout_ms (RTU_TCP also baud, frame_gap_ms,
 * broadcast_execute); UDP, RTU_UDP and ASCII_UDP use bind_address and port (RTU_UDP also
 * broadcast_execute); the PTY and DEVICE modes use the serial line fields and RTU timing (`path`
 * for DEVICE); the FD modes use `fd`, a byte stream you already own (a socketpair or PTY master),
 * never closed by the library. All modes use `seed`. The HTTP fields are independent of the mode.
 * A server with an `id` serves the devices that list it (mogbus_device_add_server()); a server
 * without one serves the devices that list no server, which is every device unless some were
 * assigned. The devices are picked when the server is created. A context holds up to
 * MOGBUS_MAX_SERVERS servers, which mogbus_step() and mogbus_run() drive together.
 *
 * `show_registers_ms` is a setting of the context rather than of the server: while any live
 * server has it set, mogbus_step() (and so mogbus_run()) shows the points of every device of
 * the context at the smallest interval asked for. On a terminal that is one table redrawn in
 * place, with the log lines in a pane below it; otherwise (a pipe, a file, or a JSON log
 * format) the full snapshot is printed as text or JSON lines, then only the changes. Read
 * values go to stdout, not through mogbus_set_log_sink().
 */
struct mogbus_server_options_t
{
	mogbus_mode_t	mode;			/**< transport */
	const char*	bind_address;		/**< TCP-family and UDP modes: an IP address or a host name (resolved at creation, IPv4 first); "0.0.0.0" opts in to remote access */
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
	bool		tls_enabled;		/**< MOGBUS_MODE_TCP only; needs a build with TLS (creation fails otherwise) */
	uint32_t	tls_handshake_timeout_ms;	/**< close unfinished handshakes after this */
	const char*	tls_certificate;	/**< server certificate chain, PEM file */
	const char*	tls_private_key;	/**< server private key, PEM file */
	const char*	tls_ca_certificate;	/**< NULL = do not verify clients */
	bool		tls_require_client_certificate;	/**< reject clients without a certificate */
	mogbus_tls_version_t tls_min_version;	/**< minimum TLS version */
	const char*	tls_ciphers_1_2;	/**< NULL = OpenSSL default */
	const char*	tls_ciphersuites_1_3;	/**< NULL = OpenSSL default */
	const char*	http_bind_address;	/**< HTTP /healthz, /readyz, /metrics endpoint: an IP address or a host name; NULL = off */
	uint16_t	http_port;		/**< endpoint port, used when http_bind_address is set */
	bool		http_control;		/**< serve the runtime control API under /api/ (needs the endpoint; no authentication) */
	bool		http_ui;		/**< serve the web UI on `/` (needs the endpoint; implies http_control) */
	const char*	http_allowed_hosts;	/**< extra Host names /api/ and the UI accept (loopback and the bind address always pass): up to 8 comma-separated names; NULL = none */
	const char*	id;			/**< names the server: unique among the live servers of the context, at most 31 bytes; prefixes its log lines; NULL = none */
	uint32_t	show_registers_ms;	/**< live register view of the context, 10 to 60000 ms; 0 = off (see below) */
	bool		char_timing;		/**< RTU byte streams: date every character (t1.5 and t3.5 between them) and, off a real serial port, send replies at the line rate; needs `baud` (a device may take it from the tty); supersedes `strict_t15` */
};

/**
 * @brief Returns the defaults of a config file.
 *
 * TCP on 127.0.0.1:1502, no idle timeout, 8N1, TLS off (when enabled: handshake timeout
 * 10000 ms, client certificates required, TLS 1.2 minimum), fd -1.
 * @return Default options.
 */
MOGBUS_API mogbus_server_options_t	mogbus_server_options_defaults(void);

/**
 * @brief Starts a server for the devices assigned to it, which is every device of the context
 *        unless mogbus_device_add_server() was used.
 *
 * With `options->id` set the server serves the devices that list that id; without it, the
 * devices that list none. Devices added or assigned later are not served by this server.
 *
 * @param ctx Context with at least one device for this server.
 * @param options Server settings; strings are copied.
 * @param out Receives the server, or NULL on failure.
 * @return MOGBUS_OK, or the reason the options are invalid or the transport cannot start (port in
 *         use, serial device missing, TLS files unreadable, no devices, MOGBUS_MAX_SERVERS
 *         reached): MOGBUS_ERR_INVALID, MOGBUS_ERR_SERIAL, MOGBUS_ERR_LISTEN or MOGBUS_ERR_NOMEM.
 *         The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_server_create(mogbus_t* ctx, const mogbus_server_options_t* options,
						     mogbus_server_t** out);

/**
 * @brief Starts a server with the (server ...) section of the last mogbus_load_config().
 * @param ctx Context with a loaded config.
 * @param out Receives the server, or NULL on failure.
 * @return MOGBUS_OK, MOGBUS_ERR_INVALID if no config is loaded, MOGBUS_ERR_UNSUPPORTED without
 *         config support, or see mogbus_server_create(). The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_server_create_from_config(mogbus_t* ctx, mogbus_server_t** out);

/**
 * @brief Returns how many (server ...) sections the last mogbus_load_config() read.
 * @param ctx Context.
 * @return The count (at least 1 after a successful load: a file without (server ...) has one
 *         default server), or 0 if no config is loaded or the build has no config support.
 */
MOGBUS_API size_t	mogbus_config_server_count(const mogbus_t* ctx);

/**
 * @brief Starts the server number @p index of the last mogbus_load_config(), serving the devices
 *        assigned to it.
 *
 * Index 0 is mogbus_server_create_from_config().
 * @param ctx Context with a loaded config.
 * @param index Position of the (server ...) section, below mogbus_config_server_count().
 * @param out Receives the server, or NULL on failure.
 * @return MOGBUS_OK, or see mogbus_server_create_from_config(); an index out of range fails with
 *         MOGBUS_ERR_INVALID.
 */
MOGBUS_API mogbus_result_t	mogbus_server_create_from_config_at(mogbus_t* ctx, size_t index,
								    mogbus_server_t** out);

/**
 * @brief Returns the (server ...) section of the last mogbus_load_config() as options.
 * @param ctx Context.
 * @param out Receives the options; strings are borrowed from the context.
 * @return MOGBUS_OK, MOGBUS_ERR_INVALID if no config is loaded, or MOGBUS_ERR_UNSUPPORTED without
 *         config support. The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_config_server_options(mogbus_t* ctx, mogbus_server_options_t* out);

/**
 * @brief Returns the server section number @p index of the last mogbus_load_config() as options.
 * @param ctx Context.
 * @param index Position of the section, below mogbus_config_server_count(); 0 is
 *        mogbus_config_server_options().
 * @param out Receives the options; strings are borrowed from the context.
 * @return MOGBUS_OK, MOGBUS_ERR_INVALID if no config is loaded or @p index is out of range, or
 *         MOGBUS_ERR_UNSUPPORTED without config support. The message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_config_server_options_at(mogbus_t* ctx, size_t index,
								mogbus_server_options_t* out);

/**
 * @brief Validates options exactly as mogbus_server_create() would, without starting anything.
 *
 * Checks the device path, fd, serial line, string lengths and TLS files (through a throwaway TLS
 * context), but opens no listener, serial device or PTY, so a port in use still passes.
 *
 * @param ctx Context.
 * @param options Server settings.
 * @return MOGBUS_OK if mogbus_server_create() would accept the options, else MOGBUS_ERR_INVALID
 *         or MOGBUS_ERR_LISTEN (TLS setup); the message is in mogbus_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_server_check(mogbus_t* ctx, const mogbus_server_options_t* options);

/**
 * @brief Checks that a server answers: one FC 08 Return Query Data round trip.
 *
 * Frames the request like the mode does and works for the TCP, UDP, RTU-over-TCP/UDP and
 * ASCII-over-TCP/UDP modes. A bind of 0.0.0.0 or :: is reached through loopback. A TLS server
 * is only connected to, since the probe has no client certificate. A fault that drops or delays
 * replies on @p unit_id makes the probe fail. It blocks the calling thread, so it cannot probe a
 * server stepped by the same thread: call it from another thread or process. It needs no context
 * or config, uses blocking sockets with a deadline and does not allocate. It is the client side of
 * `mogbus --health`.
 *
 * @param options Settings of the server to probe, e.g. from mogbus_config_server_options().
 * @param unit_id Unit to ask; not 0 (broadcast is never answered).
 * @param timeout_ms Time to wait for the reply.
 * @return MOGBUS_OK for the echo or a Modbus exception reply; MOGBUS_ERR_INVALID for a serial or
 *         fd mode, unit 0, a bind address that is neither an IP address nor a resolvable host name or NULL options;
 *         MOGBUS_ERR_PROBE for a timeout, a refused connection or a malformed reply. With no
 *         context the message is kept per thread: mogbus_last_error(NULL).
 */
MOGBUS_API mogbus_result_t	mogbus_probe(const mogbus_server_options_t* options, uint8_t unit_id,
					     uint32_t timeout_ms);

/**
 * @brief Milliseconds of a monotonic clock, the one the servers use for timeouts and faults.
 *
 * Only differences are meaningful; it never jumps when the wall clock is changed.
 *
 * @return The current time in milliseconds.
 */
MOGBUS_API uint64_t	mogbus_now_ms(void);

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

/** @brief The ADU format of a captured frame, for mogbus_decode_frame(). */
enum mogbus_framing_t
{
	MOGBUS_FRAMING_AUTO,	/**< detect it: ':' and a valid LRC is ASCII, protocol id 0 and a matching length is MBAP, a valid CRC is RTU */
	MOGBUS_FRAMING_MBAP,	/**< Modbus TCP/UDP: 7-byte MBAP header and PDU */
	MOGBUS_FRAMING_RTU,	/**< address, PDU, CRC (low byte first) */
	MOGBUS_FRAMING_ASCII,	/**< ':', hex of address, PDU and LRC, CR LF */
};

/** @brief Whether a captured PDU is a request or a response, for mogbus_decode_frame(). */
enum mogbus_direction_t
{
	MOGBUS_DIRECTION_AUTO,		/**< infer it from the length and the byte count */
	MOGBUS_DIRECTION_REQUEST,
	MOGBUS_DIRECTION_RESPONSE,
};

/** @brief Output buffer size that always holds the decoding of one frame (text or JSON, even an ambiguous one). */
#define MOGBUS_DECODE_BUFFER_SIZE	65536

/**
 * @brief Decodes one captured Modbus frame into readable text or one JSON object.
 *
 * The output lists the ADU fields (MBAP transaction/protocol/length/unit, RTU or ASCII address and
 * CRC or LRC with whether it matches), the function code and its name, the direction, and every
 * field of the PDU of each function code mogbus supports (sub-function names, file records,
 * device identification objects, CANopen, exception names); other function codes print their data
 * raw. With MOGBUS_FRAMING_AUTO or MOGBUS_DIRECTION_AUTO the output says why it chose; a frame
 * valid in several framings prints each candidate. A damaged frame (bad checksum, wrong length,
 * truncated fields) still prints every field that could be read, then lists the problems in
 * `errors` (and `ok` is false). Values a server would refuse are listed in `warnings`. The offsets
 * in messages count bytes of the frame (of the decoded bytes for ASCII). The text is JSON when
 * @p json is true: compact, one object, no trailing newline; the text format ends each line with a
 * newline. It needs no context, does not allocate and may be called from any thread.
 *
 * @param data The frame bytes (for ASCII the characters, `:` first).
 * @param size Bytes in @p data, at most 4096.
 * @param framing The ADU format, or MOGBUS_FRAMING_AUTO.
 * @param direction Request or response, or MOGBUS_DIRECTION_AUTO.
 * @param json true for a JSON object, false for text.
 * @param out Receives the NUL-terminated output.
 * @param cap Size of @p out; MOGBUS_DECODE_BUFFER_SIZE always suffices.
 * @param len Receives the output length without the NUL.
 * @return MOGBUS_OK; MOGBUS_ERR_FRAME if the frame is malformed (the output is complete);
 *         MOGBUS_ERR_NOMEM if @p out was too small (the output is cut); MOGBUS_ERR_INVALID for
 *         NULL arguments, @p cap 0 or @p size above 4096.
 */
MOGBUS_API mogbus_result_t	mogbus_decode_frame(const uint8_t* data, size_t size, mogbus_framing_t framing,
						    mogbus_direction_t direction, bool json, char* out,
						    size_t cap, size_t* len);

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

/** @brief Event bits of mogbus_poll_fd_t.events. */
enum
{
	MOGBUS_POLL_IN  = 1,	/**< wait until the descriptor is readable */
	MOGBUS_POLL_OUT = 2,	/**< wait until the descriptor is writable */
};

/**
 * @brief One descriptor a host event loop must watch for a server, see mogbus_server_poll_fds().
 *
 * `fd` is a file descriptor on POSIX and a SOCKET on Windows, carried in 64 bits either way.
 */
struct mogbus_poll_fd_t
{
	uint64_t	fd;		/**< descriptor (POSIX) or SOCKET (Windows); never an invalid one */
	uint32_t	events;		/**< MOGBUS_POLL_IN and/or MOGBUS_POLL_OUT */
};

/**
 * @brief Tells what to wait on before the next mogbus_server_step(), so a host event loop (asyncio,
 *        libuv, GLib, ...) can wait itself instead of blocking in a step.
 *
 * Fills @p out with the descriptors the server is waiting for now (listeners, client
 * connections, the datagram socket or serial fd, and those of its HTTP endpoint) and
 * @p timeout_ms with the longest the host may wait: the next deadline of the server (idle
 * timeouts, delayed responses, frame timing; at most 1000). When any descriptor is ready or the
 * timeout elapsed, call mogbus_server_step(server, 0), which then does not block. The set
 * changes with the connections, so ask again before every wait. The timeout is 0 when the
 * server was asked to stop or has finished, so that the next step reports MOGBUS_STEP_DONE.
 * The call does no I/O and, like the other calls, holds the context lock.
 *
 * @param server Server.
 * @param out Receives the descriptors; may be NULL when @p cap is 0.
 * @param cap Entries of @p out.
 * @param count Receives the number of descriptors; when it is above @p cap nothing was copied
 *        and the result is MOGBUS_ERR_INVALID (retry with a larger array; 64 entries always fit one server).
 * @param timeout_ms Receives the timeout in milliseconds.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID for a NULL argument or too small an array.
 */
MOGBUS_API mogbus_result_t	mogbus_server_poll_fds(mogbus_server_t* server, mogbus_poll_fd_t* out,
						       size_t cap, size_t* count, int* timeout_ms);

/**
 * @brief Asks the server to stop; the next step returns MOGBUS_STEP_DONE.
 *
 * Async-signal-safe and lock-free: it only sets a flag, so it may come from any thread or a
 * signal handler. A step that is waiting for I/O notices within one second (at most the
 * @p max_wait_ms of that step, or the next timer of the server, if shorter).
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
 * @brief Runs one poll iteration of every server of the context with a single wait.
 *
 * Like mogbus_server_step(), but one call serves all live servers, so a host (or
 * mogbus_run()) can drive TCP, UDP, serial and other servers from one thread. A server that
 * stopped or whose byte-stream peer went away is skipped from then on. Everything said for
 * mogbus_server_step() about threads applies to the whole context.
 *
 * @param ctx Context.
 * @param max_wait_ms Longest time to block: 0 = do not block, -1 = until the nearest deadline of
 *        any server (at most 1 s).
 * @return MOGBUS_STEP_CONTINUE while some server is live, MOGBUS_STEP_DONE once every one has
 *         stopped or finished (also with no server), MOGBUS_STEP_ERROR if a wait failed.
 */
MOGBUS_API int		mogbus_step(mogbus_t* ctx, int max_wait_ms);

/**
 * @brief mogbus_server_poll_fds() for the whole context: the descriptors of every live server and
 *        the nearest deadline, to be followed by mogbus_step(ctx, 0).
 *
 * The timeout also honours the register view of the context (see mogbus_server_options_t), which
 * mogbus_step() draws; it is 0 when no live server is left, so that the next mogbus_step()
 * returns MOGBUS_STEP_DONE. The descriptors of one context never exceed 512.
 *
 * @param ctx Context.
 * @param out Receives the descriptors; may be NULL when @p cap is 0.
 * @param cap Entries of @p out.
 * @param count Receives the number of descriptors (see mogbus_server_poll_fds()).
 * @param timeout_ms Receives the timeout in milliseconds.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID for a NULL argument or too small an array.
 */
MOGBUS_API mogbus_result_t	mogbus_poll_fds(mogbus_t* ctx, mogbus_poll_fd_t* out, size_t cap,
						size_t* count, int* timeout_ms);

/**
 * @brief Steps the whole context until every server stopped or finished, then closes the
 *        transports.
 *
 * Still call mogbus_server_close() or mogbus_destroy() afterwards.
 * @param ctx Context.
 * @return 0 after a clean stop, 1 on a poll error.
 */
MOGBUS_API int		mogbus_run(mogbus_t* ctx);

/**
 * @brief Asks every server of the context to stop; the next mogbus_step() returns
 *        MOGBUS_STEP_DONE.
 *
 * Async-signal-safe, like mogbus_server_stop().
 * @param ctx Context.
 */
MOGBUS_API void		mogbus_stop(mogbus_t* ctx);

/**
 * @brief Makes SIGINT and SIGTERM (Ctrl-C on a console) call mogbus_stop() on the context held
 *        in *slot, and ignores SIGPIPE.
 *
 * Does nothing while *slot is NULL. Process-wide, like mogbus_stop_on_signals(), which it may be
 * combined with; the slot must stay valid for the life of the process.
 * @param slot Host variable that holds the context to stop.
 */
MOGBUS_API void		mogbus_stop_context_on_signals(mogbus_t* volatile* slot);

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
 * Client connections are dropped; fds you passed in are left open. The slot is reused by the next
 * mogbus_server_create().
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

/** @brief Which way a frame seen by a frame hook was going. */
enum mogbus_frame_direction_t
{
	MOGBUS_FRAME_RECEIVED,	/**< a request frame, before it is validated or decoded */
	MOGBUS_FRAME_SENT,	/**< a response frame, before it is written or queued */
};

/**
 * @brief A transport frame, as seen by a frame hook.
 *
 * `data` is the frame as it travels on the wire: the MBAP ADU (7-byte header plus PDU) in
 * MOGBUS_MODE_TCP and MOGBUS_MODE_UDP, the RTU frame (unit id, PDU, CRC) in the RTU modes, and the
 * ASCII frame from the colon through CR LF in the ASCII modes. The hook may rewrite the bytes in
 * place. New fields are appended, so older code that ignores them still works.
 */
struct mogbus_frame_t
{
	mogbus_frame_direction_t direction;	/**< received or sent */
	mogbus_mode_t	mode;		/**< transport of the server */
	const char*	peer;		/**< client text as in the request log line ("ip:port"); "" on a serial byte stream; valid only during the call */
	uint8_t*	data;		/**< frame bytes; rewrite in place */
	size_t		len;		/**< bytes in `data` */
	size_t		capacity;	/**< most bytes `data` can hold: 260 (MBAP), 256 (RTU), 513 (ASCII) */
};

/**
 * @brief Frame hook: sees every transport frame a server receives or sends and may rewrite or drop it.
 *
 * It runs on the thread that steps the server, may call the `mogbus_device_*` functions, must not
 * close or step the server, and must not block. The frame and `peer` are valid only during the call.
 *
 * @param user Pointer given to mogbus_server_set_frame_hook().
 * @param server Server that handled the frame.
 * @param frame The frame; rewrite `data` in place (up to `capacity` bytes).
 * @return The new frame length: @p frame->len to pass it on (rewritten or not), another length up
 *         to `capacity` for a rewritten frame of that size, or 0 to drop the frame. A length above
 *         `capacity` drops the frame and logs a warning.
 */
typedef size_t (*mogbus_frame_fn)(void* user, mogbus_server_t* server, mogbus_frame_t* frame);

/**
 * @brief Installs (or, with @p fn NULL, removes) the frame hook of a server.
 *
 * pymodbus offers `trace_packet`; this shows each frame the transport receives and sends, so a host
 * can log traffic, rewrite a request or answer (a changed address, a corrupted checksum) or drop it.
 *
 * Received frames: called once per complete frame as the transport delimits it, before any validation
 * or decoding: the MBAP ADU for TCP (also with TLS, as plaintext) and UDP (every datagram that is
 * not empty or oversized, whether or not it is well formed), the RTU frame for the RTU modes
 * (on byte streams and RTU-over-TCP only frames whose CRC is valid reach the hook, because the CRC is
 * what delimits them; a UDP datagram is passed whole), the ASCII frame from the colon through CR LF
 * for the ASCII modes. Bytes the framer discards before a frame exists (resynchronization garbage,
 * partial-frame timeouts, CR/LF/space between frames) are not shown. The length the hook returns
 * replaces the frame length, and the rewritten bytes are then validated and decoded exactly as if
 * they had arrived (a hook can corrupt a CRC, change an address or a unit id). A TCP frame whose
 * rewritten MBAP header is malformed is ignored and counted as rejected, and the connection stays open.
 * Returning 0 drops the frame: it counts as never received (no processing, no counters, no response).
 *
 * Sent frames: called once per complete response frame, after fault injection (a corrupt fault is
 * already applied) and just before it is written or queued; with a delay fault the hook runs when
 * the response is built, not when the delayed bytes leave. It is not called when there is no
 * response (broadcast, drop fault, listen-only, bad CRC, ...). Returning 0 drops the response: nothing is
 * sent, the TCP connection stays open, and the metrics already recorded for the request are unchanged.
 *
 * The frame holds at most `capacity` bytes (the transport buffer: 260 for MBAP, 256 for RTU, 513 for
 * ASCII), so a rewrite may lengthen it up to that. HTTP endpoint traffic is not shown. A server has
 * one hook; a new one replaces it. The hook is not part of a config and is forgotten when the server
 * is closed.
 *
 * @param server Server.
 * @param fn Hook, or NULL to remove it.
 * @param user Passed back to @p fn.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID if @p server is NULL or already closed. The message
 *         is in mogbus_last_error() of the server's context.
 */
MOGBUS_API mogbus_result_t	mogbus_server_set_frame_hook(mogbus_server_t* server, mogbus_frame_fn fn, void* user);

/** @brief Why a client connection closed, as seen by a connection hook. */
enum mogbus_close_reason_t
{
	MOGBUS_CLOSE_NONE,		/**< not a close event: the connection opened */
	MOGBUS_CLOSE_PEER,		/**< the client closed it (end of stream or hang-up) */
	MOGBUS_CLOSE_IDLE_TIMEOUT,	/**< no traffic for the server's idle timeout */
	MOGBUS_CLOSE_DISCONNECT_FAULT,	/**< a disconnect fault fired */
	MOGBUS_CLOSE_TLS,		/**< TLS setup failed, or the handshake failed or timed out */
	MOGBUS_CLOSE_SERVER_STOP,	/**< the server was closed while the client was connected */
	MOGBUS_CLOSE_ERROR,		/**< socket error, or a protocol violation (bad MBAP header, full receive buffer) */
};

/**
 * @brief A client connection opening or closing, as seen by a connection hook.
 *
 * New fields are appended, so older code that ignores them still works.
 */
struct mogbus_connection_t
{
	bool		connected;	/**< true when the connection opened, false when it closed */
	uint64_t	id;		/**< per-server id, the same in the open and the close event, never reused by the server */
	mogbus_mode_t	mode;		/**< transport of the server */
	const char*	peer;		/**< client text as in the log lines ("ip:port"); valid only during the call */
	mogbus_close_reason_t reason;	/**< why it closed; MOGBUS_CLOSE_NONE for an open event */
};

/**
 * @brief Connection hook: told when a client connects to or leaves a server.
 *
 * It runs on the thread that steps (or closes) the server with the context lock held, may call
 * `mogbus_*` functions, must not block, and must not close or step the server. The event and `peer`
 * are valid only during the call.
 *
 * @param user Pointer given to mogbus_server_set_connection_hook().
 * @param server Server the client belongs to.
 * @param event The event.
 */
typedef void (*mogbus_connection_fn)(void* user, mogbus_server_t* server, const mogbus_connection_t* event);

/**
 * @brief Installs (or, with @p fn NULL, removes) the connection hook of a server.
 *
 * The counterpart of the frame hook, like pymodbus `trace_connect`. It is called for the connection
 * oriented transports MOGBUS_MODE_TCP (also with TLS), MOGBUS_MODE_RTU_TCP and MOGBUS_MODE_ASCII_TCP;
 * on the others (UDP, serial, PTY, fd) there are no connections, so installing it succeeds and it
 * never fires.
 *
 * An open event is sent when the server accepts the socket, before any data or TLS handshake. A client
 * refused for lack of slots is never accepted and is not reported. A close event follows exactly once
 * for every connection announced, with the reason: a TLS handshake failure or timeout closes with
 * MOGBUS_CLOSE_TLS (after the open event), and closing the server (mogbus_server_close(),
 * mogbus_destroy(), the end of mogbus_run()) reports every still connected client with
 * MOGBUS_CLOSE_SERVER_STOP. Connections open when the hook is installed are not announced and
 * produce no close event; a hook removed before a close misses it. A server has one hook; a new one
 * replaces it, and it is forgotten when the server is closed.
 *
 * @param server Server.
 * @param fn Hook, or NULL to remove it.
 * @param user Passed back to @p fn.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID if @p server is NULL or already closed.
 */
MOGBUS_API mogbus_result_t	mogbus_server_set_connection_hook(mogbus_server_t* server, mogbus_connection_fn fn, void* user);

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

/* ---- Client ---- */

/** @brief A Modbus client: one connection to one endpoint. See mogbus_client_open(). */
typedef struct mogbus_client_t		mogbus_client_t;

/**
 * @brief Client settings. Start from mogbus_client_options_defaults(); fields a mode does not use
 *        are ignored.
 *
 * Modes: MOGBUS_MODE_TCP, MOGBUS_MODE_UDP, MOGBUS_MODE_RTU_TCP, MOGBUS_MODE_RTU_UDP,
 * MOGBUS_MODE_ASCII_TCP and MOGBUS_MODE_ASCII_UDP use host and port; MOGBUS_MODE_RTU_DEVICE and
 * MOGBUS_MODE_ASCII_DEVICE use the serial line fields. Every other mode is rejected.
 *
 * TLS (MOGBUS_MODE_TCP only, a build with TLS): set tls_enabled; the server certificate is checked
 * against tls_ca_file (the system trust store when NULL) and tls_server_name (host when NULL), and
 * tls_cert_file/tls_key_file present a client certificate. Port 0 then means 802.
 */
struct mogbus_client_options_t
{
	mogbus_mode_t	mode;		/**< transport */
	const char*	host;		/**< network modes: IP literal or host name (resolved at connect time) */
	uint16_t	port;		/**< network modes; 0 is 802 with TLS */
	const char*	path;		/**< serial modes: the device, e.g. /dev/ttyUSB0 */
	uint32_t	baud;		/**< serial modes; 0 keeps the device's speed */
	uint32_t	data_bits;	/**< serial modes: 7 or 8 (RTU always uses 8) */
	mogbus_parity_t	parity;		/**< serial modes */
	uint32_t	stop_bits;	/**< serial modes: 1 or 2 */
	bool		tls_enabled;	/**< TLS over TCP: MOGBUS_MODE_TCP only; MOGBUS_ERR_UNSUPPORTED in a build without TLS */
	uint32_t	timeout_ms;	/**< how long to wait for each reply */
	uint32_t	retries;	/**< extra attempts after a timeout; other errors are never retried */
	uint32_t	byte_timeout_ms; /**< stream modes: longest gap between the bytes of one reply, 0 = off */
	const char*	tls_ca_file;	/**< TLS: PEM file of the CA(s) trusted; NULL = the system trust store */
	const char*	tls_cert_file;	/**< TLS: PEM client certificate, or NULL */
	const char*	tls_key_file;	/**< TLS: PEM private key; NULL = the key is in tls_cert_file */
	const char*	tls_server_name; /**< TLS: name to verify and send as SNI; NULL = host */
	bool		tls_verify;	/**< TLS: verify the server certificate (the default); false trusts anyone */
};

/**
 * @brief Returns the client defaults.
 *
 * TCP to 127.0.0.1:1502, 1000 ms timeout, no byte timeout, no retries, 8N1, TLS off (with
 * certificate verification on).
 * @return Default options.
 */
MOGBUS_API mogbus_client_options_t	mogbus_client_options_defaults(void);

/**
 * @brief Creates a Modbus client (master) for one endpoint.
 *
 * A client is independent of every mogbus_t and server: it only talks to an endpoint over the
 * wire, so it may live in the same process as the server it queries, on another thread. It owns
 * its memory (released by mogbus_client_close()) and connects lazily on the first call, and again
 * on the first call after a connection error. A client may be used from any thread, but only one
 * thread at a time; use one client per thread to talk in parallel. A host name is resolved at
 * connect time, which blocks for as long as the resolver takes. The library's logging (mogbus_set_log_sink()) stays process-wide.
 *
 * @param options Settings; strings are copied. Start from mogbus_client_options_defaults().
 * @param out Receives the client, or NULL on failure.
 * @return MOGBUS_OK, MOGBUS_ERR_INVALID for invalid options (unknown or server-only mode, a host
 *         that is not an IP address, no port, a bad serial line), MOGBUS_ERR_UNSUPPORTED for TLS or
 *         serial lines on a platform without them, or MOGBUS_ERR_NOMEM. With no client yet to own
 *         the message it is kept per thread, like the one of mogbus_probe():
 *         mogbus_last_error(NULL).
 */
MOGBUS_API mogbus_result_t	mogbus_client_open(const mogbus_client_options_t* options,
						   mogbus_client_t** out);

/**
 * @brief Closes the connection and frees the client.
 * @param client Client to close; NULL is ignored.
 */
MOGBUS_API void		mogbus_client_close(mogbus_client_t* client);

/**
 * @brief Message of the most recent failed call on a client.
 *
 * Every failing call overwrites it; a successful call leaves it unchanged. The pointer stays valid
 * until the next failing call on @p client or mogbus_client_close().
 *
 * @param client Client.
 * @return The message, "" if nothing failed yet.
 */
MOGBUS_API const char*	mogbus_client_last_error(const mogbus_client_t* client);

/**
 * @brief Exception code of the most recent MOGBUS_ERR_EXCEPTION on a client.
 *
 * 1 illegal function, 2 illegal data address, 3 illegal data value, 4 server failure, 11 gateway
 * target failed to respond; the server may use others.
 *
 * @param client Client.
 * @return The code, 0 if no call was answered with an exception yet.
 */
MOGBUS_API uint8_t	mogbus_client_last_exception(const mogbus_client_t* client);

/**
 * @brief Connects now instead of at the first call.
 *
 * Useful to find out early that the endpoint is not there. A no-op when already connected.
 *
 * @param client Client.
 * @return MOGBUS_OK, MOGBUS_ERR_INVALID (NULL client) or MOGBUS_ERR_IO (refused, unreachable,
 *         serial device missing or busy). The message is in mogbus_client_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_client_connect(mogbus_client_t* client);

/**
 * @brief Sends any request and returns the reply, for function codes the typed calls do not cover
 *        (including user-defined ones).
 *
 * @p data follows the function code in the request; the reply data (without the function code) is
 * copied to @p reply. A reply with the exception bit set is MOGBUS_ERR_EXCEPTION, with
 * mogbus_client_last_exception() holding the code. In the RTU and ASCII modes unit 0 is the
 * broadcast address: the request is sent, nothing is awaited and @p reply_len is 0.
 *
 * Every call that takes a @p unit uses it as the server's unit id: 1-255 in the MBAP modes and
 * 1-247 in the RTU and ASCII modes (0 broadcasts there, and only writes make sense).
 *
 * @param client Client.
 * @param unit Unit id to ask.
 * @param function_code Function code of the request, 1-127.
 * @param data Request data after the function code, may be NULL when @p data_len is 0.
 * @param data_len Bytes of @p data, at most 252.
 * @param reply Receives the reply data after the function code; may be NULL when @p reply_cap is 0.
 * @param reply_cap Size of @p reply.
 * @param reply_len Receives the reply data length; set to 0 on failure. Required.
 * @return MOGBUS_OK; MOGBUS_ERR_INVALID for a NULL client or argument or an out-of-range value;
 *         MOGBUS_ERR_IO (connect, send or receive failed, or the server closed the connection:
 *         the next call reconnects); MOGBUS_ERR_TIMEOUT; MOGBUS_ERR_EXCEPTION; MOGBUS_ERR_FRAME
 *         (malformed reply, wrong function code, or a reply longer than @p reply_cap). The
 *         message is in mogbus_client_last_error().
 */
MOGBUS_API mogbus_result_t	mogbus_client_request(mogbus_client_t* client, uint8_t unit, uint8_t function_code,
						      const uint8_t* data, size_t data_len,
						      uint8_t* reply, size_t reply_cap, size_t* reply_len);

/**
 * @brief Reads coils (function code 01).
 * @param client Client.
 * @param unit Unit id.
 * @param address First coil.
 * @param count Number of coils, 1-2000.
 * @param values Receives @p count values, one byte each, 0 or 1.
 * @return MOGBUS_OK or an error as for mogbus_client_request().
 */
MOGBUS_API mogbus_result_t	mogbus_client_read_coils(mogbus_client_t* client, uint8_t unit, uint16_t address,
							 uint16_t count, uint8_t* values);

/**
 * @brief Reads discrete inputs (function code 02).
 * @param client Client.
 * @param unit Unit id.
 * @param address First input.
 * @param count Number of inputs, 1-2000.
 * @param values Receives @p count values, one byte each, 0 or 1.
 * @return MOGBUS_OK or an error as for mogbus_client_request().
 */
MOGBUS_API mogbus_result_t	mogbus_client_read_discrete_inputs(mogbus_client_t* client, uint8_t unit,
								   uint16_t address, uint16_t count, uint8_t* values);

/**
 * @brief Reads holding registers (function code 03).
 * @param client Client.
 * @param unit Unit id.
 * @param address First register.
 * @param count Number of registers, 1-125.
 * @param values Receives @p count raw 16-bit values.
 * @return MOGBUS_OK or an error as for mogbus_client_request().
 */
MOGBUS_API mogbus_result_t	mogbus_client_read_holding_registers(mogbus_client_t* client, uint8_t unit,
								     uint16_t address, uint16_t count, uint16_t* values);

/**
 * @brief Reads input registers (function code 04).
 * @param client Client.
 * @param unit Unit id.
 * @param address First register.
 * @param count Number of registers, 1-125.
 * @param values Receives @p count raw 16-bit values.
 * @return MOGBUS_OK or an error as for mogbus_client_request().
 */
MOGBUS_API mogbus_result_t	mogbus_client_read_input_registers(mogbus_client_t* client, uint8_t unit,
								   uint16_t address, uint16_t count, uint16_t* values);

/**
 * @brief Writes one coil (function code 05).
 * @param client Client.
 * @param unit Unit id; 0 broadcasts in the RTU and ASCII modes.
 * @param address Coil.
 * @param on The new state.
 * @return MOGBUS_OK or an error as for mogbus_client_request(); MOGBUS_ERR_FRAME when the echo
 *         differs from the request.
 */
MOGBUS_API mogbus_result_t	mogbus_client_write_coil(mogbus_client_t* client, uint8_t unit, uint16_t address,
							 bool on);

/**
 * @brief Writes one holding register (function code 06).
 * @param client Client.
 * @param unit Unit id; 0 broadcasts in the RTU and ASCII modes.
 * @param address Register.
 * @param value The new raw value.
 * @return MOGBUS_OK or an error as for mogbus_client_request(); MOGBUS_ERR_FRAME when the echo
 *         differs from the request.
 */
MOGBUS_API mogbus_result_t	mogbus_client_write_register(mogbus_client_t* client, uint8_t unit, uint16_t address,
							     uint16_t value);

/**
 * @brief Writes consecutive coils (function code 15).
 * @param client Client.
 * @param unit Unit id; 0 broadcasts in the RTU and ASCII modes.
 * @param address First coil.
 * @param count Number of coils, 1-1968.
 * @param values @p count values, one byte each; nonzero is on.
 * @return MOGBUS_OK or an error as for mogbus_client_request().
 */
MOGBUS_API mogbus_result_t	mogbus_client_write_coils(mogbus_client_t* client, uint8_t unit, uint16_t address,
							  uint16_t count, const uint8_t* values);

/**
 * @brief Writes consecutive holding registers (function code 16).
 * @param client Client.
 * @param unit Unit id; 0 broadcasts in the RTU and ASCII modes.
 * @param address First register.
 * @param count Number of registers, 1-123.
 * @param values @p count raw 16-bit values.
 * @return MOGBUS_OK or an error as for mogbus_client_request().
 */
MOGBUS_API mogbus_result_t	mogbus_client_write_registers(mogbus_client_t* client, uint8_t unit,
							      uint16_t address, uint16_t count, const uint16_t* values);

/**
 * @brief Changes bits of a holding register (function code 22): new = (old AND and_mask) OR
 *        (or_mask AND NOT and_mask).
 * @param client Client.
 * @param unit Unit id; 0 broadcasts in the RTU and ASCII modes.
 * @param address Register.
 * @param and_mask Bits to keep.
 * @param or_mask Bits to set where @p and_mask is 0.
 * @return MOGBUS_OK or an error as for mogbus_client_request().
 */
MOGBUS_API mogbus_result_t	mogbus_client_mask_write_register(mogbus_client_t* client, uint8_t unit,
								  uint16_t address, uint16_t and_mask, uint16_t or_mask);

/**
 * @brief Writes holding registers, then reads holding registers, in one request (function code 23).
 * @param client Client.
 * @param unit Unit id.
 * @param read_address First register to read.
 * @param read_count Registers to read, 1-125.
 * @param read_values Receives @p read_count raw values.
 * @param write_address First register to write.
 * @param write_count Registers to write, 1-121.
 * @param write_values @p write_count raw values.
 * @return MOGBUS_OK or an error as for mogbus_client_request().
 */
MOGBUS_API mogbus_result_t	mogbus_client_read_write_registers(mogbus_client_t* client, uint8_t unit,
								   uint16_t read_address, uint16_t read_count,
								   uint16_t* read_values, uint16_t write_address,
								   uint16_t write_count, const uint16_t* write_values);

/**
 * @brief Reads the basic device identification objects (function code 43, MEI type 14).
 *
 * The objects (vendor name, product code, revision: ids 0-2) are returned as the server sent
 * them, a list of [id][length][value bytes] triples. This is
 * mogbus_client_read_device_identification() with read code 1 and object 0: replies that do not
 * fit one PDU are followed up.
 *
 * @param client Client.
 * @param unit Unit id.
 * @param out Receives the object list.
 * @param cap Size of @p out; 252 is always enough.
 * @param out_len Receives the bytes written to @p out.
 * @return MOGBUS_OK or an error as for mogbus_client_request().
 */
MOGBUS_API mogbus_result_t	mogbus_client_read_device_id(mogbus_client_t* client, uint8_t unit, uint8_t* out,
							     size_t cap, size_t* out_len);

/**
 * @brief Reads device identification objects (function code 43, MEI type 14), following up a
 *        reply that says more objects follow until the server has sent them all.
 *
 * @param client Client.
 * @param unit Unit id.
 * @param read_code 1 basic (objects 0-2), 2 regular (0-6), 3 extended (0-6, then 0x80-0xFF) or
 *        4 the one object @p object_id.
 * @param object_id First object to read (0 from the start; the object itself with code 4).
 * @param out Receives the objects as [id][length][value bytes] triples, all replies joined.
 * @param cap Size of @p out.
 * @param out_len Receives the bytes written to @p out.
 * @param conformity Receives the conformity level of the server (0x81-0x83, 0x01-0x03 for
 *        individual access); NULL to ignore.
 * @return MOGBUS_OK or an error as for mogbus_client_request(); MOGBUS_ERR_FRAME for a reply that
 *         does not match the request or the objects that do not fit @p cap.
 */
MOGBUS_API mogbus_result_t	mogbus_client_read_device_identification(mogbus_client_t* client, uint8_t unit,
									 uint8_t read_code, uint8_t object_id,
									 uint8_t* out, size_t cap, size_t* out_len,
									 uint8_t* conformity);

/**
 * @brief Reads the exception status byte (function code 07).
 *
 * @param client Client.
 * @param unit Unit id.
 * @param status Receives the status byte.
 * @return MOGBUS_OK or an error as for mogbus_client_request().
 */
MOGBUS_API mogbus_result_t	mogbus_client_read_exception_status(mogbus_client_t* client, uint8_t unit,
								    uint8_t* status);

/**
 * @brief Sends a diagnostics request (function code 08) and returns the reply data.
 *
 * The server must echo the sub-function. Sub-function 0x0000 (Return Query Data) echoes @p data
 * (a multiple of 2 bytes for a mogbus server); the others take and return 2 bytes.
 *
 * @param client Client.
 * @param unit Unit id.
 * @param sub_function Sub-function code.
 * @param data Request data after the sub-function; may be NULL when @p data_len is 0.
 * @param data_len Bytes in @p data, at most 250.
 * @param out Receives the reply data after the sub-function.
 * @param cap Size of @p out.
 * @param out_len Receives the bytes written to @p out.
 * @return MOGBUS_OK or an error as for mogbus_client_request(); MOGBUS_ERR_FRAME when the
 *         sub-function is not echoed.
 */
MOGBUS_API mogbus_result_t	mogbus_client_diagnostics(mogbus_client_t* client, uint8_t unit,
							  uint16_t sub_function, const uint8_t* data,
							  size_t data_len, uint8_t* out, size_t cap,
							  size_t* out_len);

/**
 * @brief Reads the communication event counter (function code 0B).
 *
 * @param client Client.
 * @param unit Unit id.
 * @param status Receives the status word (0 ready, 0xFFFF busy).
 * @param event_count Receives the event counter.
 * @return MOGBUS_OK or an error as for mogbus_client_request().
 */
MOGBUS_API mogbus_result_t	mogbus_client_get_comm_event_counter(mogbus_client_t* client, uint8_t unit,
								     uint16_t* status, uint16_t* event_count);

/**
 * @brief Reads the communication event log (function code 0C).
 *
 * @param client Client.
 * @param unit Unit id.
 * @param status Receives the status word.
 * @param event_count Receives the event counter.
 * @param message_count Receives the message counter.
 * @param events Receives the event bytes (at most 64).
 * @param cap Size of @p events; 64 is always enough.
 * @param events_len Receives the number of event bytes.
 * @return MOGBUS_OK or an error as for mogbus_client_request().
 */
MOGBUS_API mogbus_result_t	mogbus_client_get_comm_event_log(mogbus_client_t* client, uint8_t unit,
								 uint16_t* status, uint16_t* event_count,
								 uint16_t* message_count, uint8_t* events,
								 size_t cap, size_t* events_len);

/**
 * @brief Reads the server id report (function code 11).
 *
 * The data is device specific; mogbus devices send the server id, the run indicator (0xFF on) and
 * their additional data, in that order.
 *
 * @param client Client.
 * @param unit Unit id.
 * @param out Receives the report data.
 * @param cap Size of @p out; 249 is always enough.
 * @param out_len Receives the bytes written to @p out.
 * @return MOGBUS_OK or an error as for mogbus_client_request().
 */
MOGBUS_API mogbus_result_t	mogbus_client_report_server_id(mogbus_client_t* client, uint8_t unit,
							       uint8_t* out, size_t cap, size_t* out_len);

/** @brief One sub-request of mogbus_client_read_file_records() or mogbus_client_write_file_records(). */
struct mogbus_client_file_record_t
{
	uint16_t	file;		/**< file number, 1-65535 */
	uint16_t	record;		/**< first record, 0-9999 */
	uint16_t	length;		/**< number of 16-bit records, at least 1 (record + length - 1 at most 9999) */
	uint16_t*	values;		/**< length values: filled by a read, written by a write */
};

/**
 * @brief Reads file records (function code 14); one reply answers every sub-request.
 *
 * @param client Client.
 * @param unit Unit id.
 * @param records 1-35 sub-requests; each `values` must have room for `length` values and is filled
 *        from the reply.
 * @param count Number of sub-requests.
 * @return MOGBUS_OK or an error as for mogbus_client_request(); MOGBUS_ERR_FRAME when the reply
 *         does not have the shape of the request.
 */
MOGBUS_API mogbus_result_t	mogbus_client_read_file_records(mogbus_client_t* client, uint8_t unit,
								const mogbus_client_file_record_t* records,
								size_t count);

/**
 * @brief Writes file records (function code 15); all or nothing.
 *
 * @param client Client.
 * @param unit Unit id.
 * @param records Sub-requests; the byte total of the request (7 + 2 x length each) is at most 251.
 * @param count Number of sub-requests, at least 1.
 * @return MOGBUS_OK or an error as for mogbus_client_request(); MOGBUS_ERR_FRAME when the reply is
 *         not an echo of the request.
 */
MOGBUS_API mogbus_result_t	mogbus_client_write_file_records(mogbus_client_t* client, uint8_t unit,
								 const mogbus_client_file_record_t* records,
								 size_t count);

/**
 * @brief Reads a FIFO queue (function code 18).
 *
 * @param client Client.
 * @param unit Unit id.
 * @param address FIFO pointer address.
 * @param values Receives the queued values.
 * @param cap Size of @p values; 31 is always enough.
 * @param count Receives the number of values (0-31).
 * @return MOGBUS_OK or an error as for mogbus_client_request().
 */
MOGBUS_API mogbus_result_t	mogbus_client_read_fifo(mogbus_client_t* client, uint8_t unit, uint16_t address,
							uint16_t* values, size_t cap, size_t* count);

/**
 * @brief Sends a CANopen general reference request (function code 2B, MEI type 0D).
 *
 * The request is the bytes after the MEI type: control byte, reserved byte 0, optional network id
 * and data type, node, index, subindex, start, count and the data of a write. On success the
 * reply is the bytes after the MEI type (control byte onward, echoing the request).
 *
 * A failure of the CANopen service (`AB FF len 0D AE|CE ...`) is a MOGBUS_ERR_EXCEPTION with
 * mogbus_client_last_exception() 0xFF; @p reply then holds the bytes after the function code
 * (`FF len 0D AE|CE payload`) so the caller can decode it. A plain Modbus exception leaves its one
 * code byte in @p reply.
 *
 * @param client Client.
 * @param unit Unit id.
 * @param request Request bytes after `2B 0D`.
 * @param request_len Bytes in @p request, 1-250.
 * @param reply Receives the reply bytes described above.
 * @param cap Size of @p reply.
 * @param reply_len Receives the bytes written to @p reply.
 * @return MOGBUS_OK or an error as for mogbus_client_request().
 */
MOGBUS_API mogbus_result_t	mogbus_client_canopen_request(mogbus_client_t* client, uint8_t unit,
							      const uint8_t* request, size_t request_len,
							      uint8_t* reply, size_t cap, size_t* reply_len);

/**
 * @brief Decodes one number from registers, the way a point of that type, word order and byte order is read.
 *
 * It reads 1 register for U16/S16, 2 for U32/S32/F32 and 4 for U64/S64/F64 (the register layouts are
 * in the table at mogbus_point_t, for example ABCD is MOGBUS_WORDS_HIGH_FIRST with MOGBUS_BYTES_BIG).
 * It is the code that serves the points, so what a client reads back with the same type and orders
 * is what mogbus_value_to_registers() wrote. 64-bit integers go through a double and lose precision
 * above 2^53: use mogbus_registers_to_u64() for those. It needs no context and no lock, and keeps no
 * error message.
 *
 * @param regs The registers, in the order they came off the wire (low address first); at least as many as the type uses.
 * @param type The register encoding; not MOGBUS_TYPE_BOOL or MOGBUS_TYPE_STRING (see mogbus_registers_to_text()).
 * @param word_order Which register of a 32- or 64-bit value comes first; ignored for 16-bit types.
 * @param byte_order MOGBUS_BYTES_LITTLE swaps the two bytes of every register.
 * @param out Receives the value.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID for a NULL argument or a type that is not numeric.
 */
MOGBUS_API mogbus_result_t	mogbus_registers_to_value(const uint16_t* regs, mogbus_value_type_t type,
							  mogbus_word_order_t word_order,
							  mogbus_byte_order_t byte_order, double* out);

/**
 * @brief Encodes one number into registers, the inverse of mogbus_registers_to_value().
 *
 * Integer types clamp to their range, round fractions and take NaN as 0; the float types keep NaN
 * and infinity, and a finite value outside the float32 range becomes an infinity.
 *
 * @param value The number.
 * @param type The register encoding; not MOGBUS_TYPE_BOOL or MOGBUS_TYPE_STRING.
 * @param word_order Which register of a 32- or 64-bit value comes first; ignored for 16-bit types.
 * @param byte_order MOGBUS_BYTES_LITTLE swaps the two bytes of every register.
 * @param regs Receives the registers: room for 4 (1, 2 or 4 are written).
 * @param count Receives how many registers were written; may be NULL.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID for a NULL @p regs or a type that is not numeric.
 */
MOGBUS_API mogbus_result_t	mogbus_value_to_registers(double value, mogbus_value_type_t type,
							  mogbus_word_order_t word_order,
							  mogbus_byte_order_t byte_order, uint16_t* regs,
							  size_t* count);

/**
 * @brief Decodes a 64-bit integer from 4 registers exactly (mogbus_registers_to_value() rounds above 2^53).
 *
 * The result is the 64-bit pattern: cast it to int64_t for a MOGBUS_TYPE_S64 value.
 *
 * @param regs 4 registers, low address first.
 * @param word_order Which register comes first.
 * @param byte_order MOGBUS_BYTES_LITTLE swaps the two bytes of every register.
 * @param out Receives the 64 bits.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID for a NULL argument.
 */
MOGBUS_API mogbus_result_t	mogbus_registers_to_u64(const uint16_t* regs, mogbus_word_order_t word_order,
							mogbus_byte_order_t byte_order, uint64_t* out);

/**
 * @brief Encodes a 64-bit integer into 4 registers exactly; cast an int64_t to uint64_t for a MOGBUS_TYPE_S64 value.
 *
 * @param value The 64 bits.
 * @param word_order Which register comes first.
 * @param byte_order MOGBUS_BYTES_LITTLE swaps the two bytes of every register.
 * @param regs Receives 4 registers.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID for a NULL @p regs.
 */
MOGBUS_API mogbus_result_t	mogbus_u64_to_registers(uint64_t value, mogbus_word_order_t word_order,
							mogbus_byte_order_t byte_order, uint16_t* regs);

/**
 * @brief Decodes text from registers (two ASCII bytes each, the first byte in the high byte unless the byte order is little).
 *
 * The text ends at the first zero byte or after 2 * @p count bytes; @p out is always terminated.
 *
 * @param regs The registers.
 * @param count How many registers hold the text.
 * @param byte_order MOGBUS_BYTES_LITTLE swaps the two bytes of every register.
 * @param out Receives the text.
 * @param cap Size of @p out; 2 * @p count + 1 holds any text.
 * @return MOGBUS_OK, MOGBUS_ERR_INVALID for a NULL argument or a zero @p cap, or MOGBUS_ERR_NOMEM when the text was cut to fit (what fits is written).
 */
MOGBUS_API mogbus_result_t	mogbus_registers_to_text(const uint16_t* regs, size_t count,
							 mogbus_byte_order_t byte_order, char* out, size_t cap);

/**
 * @brief Encodes text into registers, padding with zero bytes (the inverse of mogbus_registers_to_text()).
 *
 * @param text The text, at most 2 * @p count bytes.
 * @param count How many registers to fill, 1 to 123.
 * @param byte_order MOGBUS_BYTES_LITTLE swaps the two bytes of every register.
 * @param regs Receives @p count registers.
 * @return MOGBUS_OK, or MOGBUS_ERR_INVALID for a NULL argument, a bad @p count or text that does not fit.
 */
MOGBUS_API mogbus_result_t	mogbus_text_to_registers(const char* text, size_t count,
							 mogbus_byte_order_t byte_order, uint16_t* regs);

#ifdef __cplusplus
}
#endif

#endif
