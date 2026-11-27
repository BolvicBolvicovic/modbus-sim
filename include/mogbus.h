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
typedef enum mogbus_frame_direction_t mogbus_frame_direction_t;
typedef struct mogbus_frame_t mogbus_frame_t;
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
 * See docs/user/config/format.md.
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
};

/**
 * @brief Adds an empty device to the context.
 *
 * Points support static values, ramps and wall-clock fields. Machines, faults and user-defined
 * function codes are only available through config files (mogbus_load_config()); code-built and
 * loaded devices can be mixed in one context, which holds up to MOGBUS_MAX_DEVICES devices.
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
 * @return false if @p dev is NULL or another device of the context already has it.
 */
MOGBUS_API bool		mogbus_device_set_any_unit(mogbus_device_t* dev, bool on);

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
 * Ramps are advanced to the current time first. Bit spaces read as 0 or 1. Host side: it never
 * calls the access hook.
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
 * mogbus_device_write() to change a value the way a client would. It never calls the access hook.
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
 * @return false if @p dev is NULL or out of memory.
 */
MOGBUS_API bool		mogbus_device_set_access_hook(mogbus_device_t* dev, mogbus_access_fn fn, void* user);

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
 * @return false if @p dev is NULL or out of memory.
 */
MOGBUS_API bool		mogbus_device_set_written_hook(mogbus_device_t* dev, mogbus_written_fn fn, void* user);

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
 * @return false if @p dev or @p fn is NULL, the code is out of range, built in or already defined on
 *         the device (by config or by this function), the device already has 8 codes, or memory
 *         ran out.
 */
MOGBUS_API bool		mogbus_device_add_function_code(mogbus_device_t* dev, uint8_t code, mogbus_function_fn fn, void* user);

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
 * @param err Buffer for an error message; may be NULL.
 * @param err_size Size of @p err; 0 discards the message.
 * @return false for a NULL argument or an invalid range, an overlap, too many files (16) or records
 *         per file (64), or out of memory.
 */
MOGBUS_API bool		mogbus_device_add_file_record(mogbus_device_t* dev, uint16_t file, uint16_t record,
						      uint16_t count, const uint16_t* values,
						      char* err, size_t err_size);

/**
 * @brief Reads @p count consecutive file records starting at @p record (host side, no hook).
 * @param dev Device.
 * @param file File number.
 * @param record First record number.
 * @param count Number of records.
 * @param out Receives @p count values.
 * @return false if the file or any record is not defined, @p count is 0 or @p out is NULL.
 */
MOGBUS_API bool		mogbus_device_read_file_records(mogbus_device_t* dev, uint16_t file, uint16_t record,
							uint16_t count, uint16_t* out);

/**
 * @brief Writes @p count consecutive file records starting at @p record (host side, no hook).
 * @param dev Device.
 * @param file File number.
 * @param record First record number.
 * @param count Number of records.
 * @param values @p count values to store.
 * @return false if the file or any record is not defined, @p count is 0 or @p values is NULL;
 *         nothing is written then.
 */
MOGBUS_API bool		mogbus_device_write_file_records(mogbus_device_t* dev, uint16_t file, uint16_t record,
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
 * @param err Buffer for an error message; may be NULL.
 * @param err_size Size of @p err; 0 discards the message.
 * @return false for a duplicate address, too many values or queues (16), or out of memory.
 */
MOGBUS_API bool		mogbus_device_add_fifo(mogbus_device_t* dev, uint16_t address, uint16_t count,
					       const uint16_t* values, char* err, size_t err_size);

/**
 * @brief Replaces the contents of the FIFO queue at @p address (host side, no hook).
 *
 * Typically called from an access hook to refresh the queue just before a client reads it.
 *
 * @param dev Device.
 * @param address FIFO pointer address of an existing queue.
 * @param count New number of values, 0-31.
 * @param values @p count values; copied; may be NULL when @p count is 0.
 * @return false if no queue is defined at @p address or @p count is above 31.
 */
MOGBUS_API bool		mogbus_device_set_fifo(mogbus_device_t* dev, uint16_t address, uint16_t count,
					       const uint16_t* values);

/**
 * @brief Adds a CANopen object served by FC 2B/0D, as `(canopen-object ...)` does in a config.
 * @param dev Device.
 * @param node Node id, 1-127.
 * @param index Object index.
 * @param subindex Object subindex.
 * @param length Object size in bytes, 1-238; fixed for the object's lifetime.
 * @param data @p length initial bytes; copied.
 * @param err Buffer for an error message; may be NULL.
 * @param err_size Size of @p err; 0 discards the message.
 * @return false for an invalid node or length, a duplicate (node, index, subindex), too many
 *         objects (64) or a NULL argument.
 */
MOGBUS_API bool		mogbus_device_add_canopen_object(mogbus_device_t* dev, uint8_t node, uint16_t index,
							 uint8_t subindex, uint16_t length, const uint8_t* data,
							 char* err, size_t err_size);

/**
 * @brief Reads @p count bytes at @p offset of a CANopen object (host side, no hook).
 * @param dev Device.
 * @param node Node id.
 * @param index Object index.
 * @param subindex Object subindex.
 * @param offset First byte.
 * @param count Number of bytes.
 * @param out Receives @p count bytes.
 * @return false if the object does not exist or the range is outside its length.
 */
MOGBUS_API bool		mogbus_device_read_canopen(mogbus_device_t* dev, uint8_t node, uint16_t index,
						   uint8_t subindex, uint16_t offset, uint16_t count, uint8_t* out);

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
 * @return false if the object does not exist or the range is outside its length.
 */
MOGBUS_API bool		mogbus_device_write_canopen(mogbus_device_t* dev, uint8_t node, uint16_t index,
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
 * A server serves every device the context has when it is created; a context holds up to
 * MOGBUS_MAX_SERVERS servers.
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
	bool		tls_enabled;		/**< MOGBUS_MODE_TCP only; needs a build with TLS (creation fails otherwise) */
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
	bool		http_ui;		/**< serve the web UI on `/` (needs the endpoint; implies http_control) */
	const char*	http_allowed_hosts;	/**< extra Host names /api/ and the UI accept (loopback and the bind address always pass): up to 8 comma-separated names; NULL = none */
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
 * @brief Why a server could not be created (see mogbus_server_create()).
 *
 * The `mogbus` binary maps MOGBUS_ERR_SERIAL to exit status 1 and MOGBUS_ERR_LISTEN to 2. A TLS
 * setup failure is also logged at WARN, like the transports log their own start-up errors.
 */
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
 * server stepped by the same thread: call it from another thread or process. It needs no context
 * or config, uses blocking sockets with a deadline and does not allocate. It is the client side of
 * `mogbus --health`.
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
 * @return false if @p server is NULL.
 */
MOGBUS_API bool		mogbus_server_set_frame_hook(mogbus_server_t* server, mogbus_frame_fn fn, void* user);

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
