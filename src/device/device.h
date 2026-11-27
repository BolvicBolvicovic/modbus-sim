/**
 * @file
 * @brief Simulated Modbus device: four data tables and the points that own their addresses.
 */
#ifndef DEVICE_H
#define DEVICE_H

#include "vlib/c_types.h"
#include "vlib/arena.h"
#include "device/behaviour.h"
#include "device/encode.h"
#include "device/fault.h"
#include "device/value.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum space_kind_t space_kind_t;
typedef enum point_kind_t point_kind_t;
typedef enum clock_field_t clock_field_t;
typedef struct point_t point_t;
typedef struct space_t space_t;
typedef struct file_segment_t file_segment_t;
typedef struct device_file_t device_file_t;
typedef struct device_fifo_t device_fifo_t;
typedef struct device_canopen_object_t device_canopen_object_t;
typedef enum custom_action_t custom_action_t;
typedef struct device_custom_fc_t device_custom_fc_t;
typedef struct device_access_t device_access_t;
typedef enum access_target_t access_target_t;
typedef struct device_t device_t;
/**
 * @brief Host callback of a user-defined function code: returns 0 to reply with `reply[0..*reply_len)`
 *        (at most `cap` bytes), 1-255 to answer that exception, DEVICE_CUSTOM_NO_RESPONSE for silence.
 */
typedef int (*device_custom_fn_t)(void* user, device_t* dev, u8 unit, u8 code, const u8* data, u32 len, u8* reply, u32 cap, u32* reply_len);

#define DEVICE_NAME_MAX		64
#define DEVICE_MAX_SERVERS	8	/**< servers one device can be assigned to */
#define DEVICE_SERVER_NAME_MAX	32	/**< longest server name plus the NUL */
#define DEVICE_ID_OBJECT_MAX	244	/**< 7-byte MEI header + 2-byte object header + value <= MODBUS_MAX_PDU */
/** @brief 253-byte Modbus PDU minus FC, byte count, server ID and run indicator. */
#define DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX	249
#define DEVICE_MAX_EXTENDED_IDS	32
#define DEVICE_MAX_POINTS	8192
#define DEVICE_MAX_STRING_REGISTERS	123	/**< a whole string fits one FC 03/04 read */
#define ADDRESS_SPACE_SIZE	65536

/** @brief The four Modbus data tables. */
enum space_kind_t
{
	SPACE_COIL,	/**< read/write bit    (FC 01, 05, 15) */
	SPACE_DISCRETE,	/**< read-only bit     (FC 02)         */
	SPACE_INPUT,	/**< read-only 16 bits (FC 04)         */
	SPACE_HOLDING,	/**< read/write 16 bits(FC 03, 06, 16) */
	SPACE_COUNT,
};

/** @brief How the value of a point is produced. */
enum point_kind_t
{
	POINT_STATIC,	/**< value never changes by itself */
	POINT_RAMP,	/**< value follows a ramp over time */
	POINT_MODEL,	/**< value follows a sine or lag model, or a static value plus noise */
	POINT_MACHINE,	/**< value is driven by a scripted device machine */
	POINT_CLOCK,	/**< value is a field of the wall clock (see clock_field_t) */
};

/** @brief Which wall-clock value a POINT_CLOCK point shows. */
enum clock_field_t
{
	CLOCK_YEAR,
	CLOCK_MONTH,
	CLOCK_DAY,
	CLOCK_WEEKDAY,	/**< ISO: Monday 1 ... Sunday 7 */
	CLOCK_HOUR,
	CLOCK_MINUTE,
	CLOCK_SECOND,
	CLOCK_UNIX,	/**< seconds since the epoch, always UTC */
};

/**
 * @brief One configured data point: where it lives, how it is encoded, and how its value
 *        behaves.
 *
 * A point occupies point_width() addresses.
 */
struct point_t
{
	space_kind_t	space;
	u32		address;
	value_type_t	type;
	word_order_t	order;
	byte_order_t	byte_order;		/**< swaps the two bytes of every register */
	point_kind_t	kind;
	point_kind_t	configured_kind;	/**< kind as configured, before client writes */
	f64		static_value;
	u64		static_raw;		/**< exact VT_U64/VT_S64 static value when static_exact */
	bool		static_exact;
	bool		read_only;		/**< coil/holding only: refused by client writes (host writes allowed) */
	u16		length;			/**< VT_STRING: registers (1..DEVICE_MAX_STRING_REGISTERS) */
	char		padding;		/**< VT_STRING: fills text up to 2*length bytes (0 or ' ') */
	/**
	 * VT_STRING: initial text. device_add_point() takes a NUL-terminated string of at most
	 * 2*length bytes and replaces it with its own copy, padded to exactly 2*length bytes.
	 */
	const char*	text;
	ramp_params_t	ramp;
	clock_field_t	clock_field;		/**< POINT_CLOCK */
	bool		clock_utc;		/**< POINT_CLOCK: UTC instead of local time */
	model_t		model;			/**< POINT_MODEL shape; noise modifier of POINT_RAMP */
};

/**
 * @brief One data table.
 *
 * `owner` is the 1-based index of the point that defines each address (0 = address not
 * defined). Bit spaces store 0 or 1.
 */
struct space_t
{
	u16	value[ADDRESS_SPACE_SIZE];
	u16	owner[ADDRESS_SPACE_SIZE];
};

#define DEVICE_MAX_FILES		16
#define DEVICE_MAX_FILE_SEGMENTS	64
#define DEVICE_MAX_RECORD_VALUES	126
#define DEVICE_MAX_FIFOS		16
#define DEVICE_CUSTOM_NO_RESPONSE	(-1)	/**< device_custom_fn_t result: send nothing */
#define DEVICE_MAX_CUSTOM_FCS		8	/**< user-defined function codes per device */
#define DEVICE_MAX_CUSTOM_CASES		16	/**< (on ...) cases per user-defined code */
#define DEVICE_CUSTOM_PREFIX_MAX	16	/**< request bytes an (on "...") case matches */
#define DEVICE_CUSTOM_REPLY_MAX		252	/**< reply data after the function code */
#define DEVICE_MAX_FIFO_VALUES		31
#define DEVICE_MAX_COM_EVENTS		64
#define DEVICE_MAX_CANOPEN_OBJECTS	64
#define DEVICE_MAX_CANOPEN_OBJECT_BYTES	238

/**
 * @brief One contiguous block of 16-bit register values in a file, starting at Modbus record
 *        number `record`.
 */
struct file_segment_t
{
	u16	record;
	u16	length;
	u16*	values;	/**< length entries, arena-backed */
};

/** @brief Configured FC 20 (Read File Record) storage for one file number. */
struct device_file_t
{
	u16		file_no;
	u32		segment_count;
	file_segment_t	segments[DEVICE_MAX_FILE_SEGMENTS];
};

/**
 * @brief Configured FC 24 (Read FIFO Queue) storage for one FIFO pointer.
 *
 * Values are returned in configured order, non-destructively.
 */
struct device_fifo_t
{
	u16	address;	/**< FIFO pointer address */
	u16	count;		/**< 0..DEVICE_MAX_FIFO_VALUES */
	u16*	values;		/**< count entries, arena-backed */
};

/** @brief One CANopen object served by FC 43 MEI 0x0D SDO transfers. */
struct device_canopen_object_t
{
	u8	node_id;
	u16	index;
	u8	subindex;
	u16	length;
	u8	data[DEVICE_MAX_CANOPEN_OBJECT_BYTES];
};

/**
 * @brief FC 08 diagnostic register bits.
 *
 * The simulator has no UART character stream, so the character-overrun flag/count are always
 * clear; 0x0014 still resets them.
 */
enum diagnostic_register_bit
{
	DIAG_REG_CHARACTER_OVERRUN       = 1u << 0,
};

/** @brief MEI 14 (Read Device Identification) object ids. */
enum device_id_object
{
	DEVICE_ID_VENDOR_NAME          = 0x00,
	DEVICE_ID_PRODUCT_CODE         = 0x01,
	DEVICE_ID_MAJOR_MINOR_REVISION = 0x02,
	DEVICE_ID_VENDOR_URL           = 0x03,
	DEVICE_ID_PRODUCT_NAME         = 0x04,
	DEVICE_ID_MODEL_NAME           = 0x05,
	DEVICE_ID_USER_APPLICATION_NAME = 0x06,
	DEVICE_ID_STANDARD_COUNT       = 0x07,
};

/** @brief What a user-defined function code answers for one matching request. */
enum custom_action_t
{
	CUSTOM_REPLY,		/**< fixed reply data after the function code */
	CUSTOM_ECHO,		/**< the request data, unchanged */
	CUSTOM_EXCEPTION,	/**< exception response with `exception_code` */
	CUSTOM_REGISTERS,	/**< byte count + current values of a table range, like FC 01-04 */
	CUSTOM_NO_RESPONSE,	/**< no response at all */
};

/**
 * @brief One user-defined function code: cases matched in order on the request data (the bytes
 *        after the function code).
 *
 * A case matches when the request data starts with its prefix; an empty prefix matches every
 * request. A request no case matches gets exception 03 (illegal data value). A code the host defines
 * has `fn` set and no cases: `fn` answers every request (see device_custom_fn_t).
 */
struct device_custom_fc_t
{
	u8		code;			/**< 1-127, not a function code mogbus implements */
	char		name[DEVICE_NAME_MAX];	/**< for logs and summaries; may be empty */
	u32		case_count;
	device_custom_fn_t fn;			/**< host callback (NULL: the cases answer) */
	void*		user;			/**< passed to `fn` */
	/** @brief One `(on ...)` case of a user-defined function code. */
	struct device_custom_case_t
	{
		u8		prefix[DEVICE_CUSTOM_PREFIX_MAX];
		u8		prefix_len;
		custom_action_t	action;
		u8		exception_code;	/**< CUSTOM_EXCEPTION */
		space_kind_t	space;		/**< CUSTOM_REGISTERS */
		u16		address;	/**< CUSTOM_REGISTERS: first address */
		u16		count;		/**< CUSTOM_REGISTERS: registers (or bits) */
		u16		reply_len;	/**< CUSTOM_REPLY */
		u8		reply[DEVICE_CUSTOM_REPLY_MAX];
	} cases[DEVICE_MAX_CUSTOM_CASES];
};

/** @brief One simulated Modbus device (one unit id). */
struct machine_system_t; /* defined in device/machine.h */
struct action_system_t; /* defined in device/action.h */

/** @brief What a device_access_t touches. */
enum access_target_t
{
	ACCESS_TABLE,		/**< a range of one of the four tables */
	ACCESS_FILE_RECORD,	/**< one FC 14/15 sub-request */
	ACCESS_FIFO,		/**< an FC 18 queue read */
	ACCESS_CANOPEN,		/**< an FC 2B/0D object transfer */
};

/** @brief One Modbus client access handed to a device's access hook. */
struct device_access_t
{
	bool		write;		/**< a write (values set) rather than a read */
	space_kind_t	space;		/**< ACCESS_TABLE only */
	u32		address;	/**< first address; record number; FIFO address; CANopen start offset */
	u32		count;		/**< registers, bits, records or CANopen bytes; 0 for FIFO */
	const u16*	values;		/**< writes: the values about to be stored; reads: NULL */
	u8		function_code;
	access_target_t	target;
	u16		file_no;	/**< ACCESS_FILE_RECORD */
	u8		node_id;	/**< ACCESS_CANOPEN */
	u16		index;		/**< ACCESS_CANOPEN */
	u8		subindex;	/**< ACCESS_CANOPEN */
	const u8*	data;		/**< ACCESS_CANOPEN writes: the bytes about to be stored; reads: NULL */
	u32		data_len;	/**< bytes in `data` */
};

/** @brief One simulated device: its tables, points, faults, machines and counters. */
struct device_t
{
	char		name[DEVICE_NAME_MAX];
	u8		unit_id;
	bool		any_unit;		/**< also answers every unit id no other device has */
	u32		server_count;		/**< names in `servers`; 0 = served by the servers that have no id */
	char		servers[DEVICE_MAX_SERVERS][DEVICE_SERVER_NAME_MAX];	/**< ids of the servers that serve the device */
	void*		owner;			/**< API context that holds the device (set by the API) */
	u8		exception_status;	/**< FC 07 (Read Exception Status) response byte */
	u8		server_id;		/**< FC 17 (Report Server ID) server ID byte */
	u8		run_indicator;		/**< FC 17 Run Indicator Status (0xFF run, 0x00 off) */
	u8		server_id_additional_data[DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX];
	u8		server_id_additional_data_len;
	u16		com_status;		/**< FC 11/12 status: 0 ready, 0xFFFF busy */
	u16		com_event_count;	/**< FC 11/12 event counter, wraps modulo 65536 */
	u16		message_count;		/**< FC 12 + FC 08 bus/slave message count */
	u16		com_event_log_count;	/**< number of configured FC 12 event bytes */
	u8		com_events[DEVICE_MAX_COM_EVENTS]; /**< FC 12 event bytes, in order */
	u16		diagnostic_register;	/**< FC 08 subfunction 0x0002 bitmask */
	u16		diag_comm_error_count;	/**< FC 08 subfunction 0x000C */
	u16		diag_exception_count;	/**< FC 08 subfunction 0x000D */
	u16		diag_no_response_count;	/**< FC 08 subfunction 0x000F */
	u16		diag_nak_count;		/**< FC 08 subfunction 0x0010 */
	u16		diag_busy_count;	/**< FC 08 subfunction 0x0011 */
	u16		diag_overrun_count;	/**< FC 08 subfunction 0x0012 */
	bool		listen_only;		/**< FC 08 subfunction 0x0004 */
	u64		start_ms;		/**< ramps count their time from here */
	s64		wall_fixed_ms;		/**< nonzero pins the wall clock of clock points (tests) */
	s64		wall_ms;		/**< wall clock read by the last tick, restart or add */
	u64		now_ms;			/**< last device_tick, or start_ms before the first tick */
	fault_set_t	faults;
	fault_runtime_t	fault_runtime;
	arena_t*	arena;			/**< backing arena for dynamic config storage */
	u32		point_count;
	point_t*	points;			/**< DEVICE_MAX_POINTS entries */
	u32		file_count;
	device_file_t*	files;			/**< DEVICE_MAX_FILES entries */
	u32		fifo_count;
	device_fifo_t*	fifos;			/**< DEVICE_MAX_FIFOS entries */
	u32		canopen_object_count;
	device_canopen_object_t* canopen_objects; /**< DEVICE_MAX_CANOPEN_OBJECTS entries */
	u32		custom_fc_count;
	device_custom_fc_t* custom_fcs;	/**< DEVICE_MAX_CUSTOM_FCS entries, allocated on first use */
	char		device_id[DEVICE_ID_STANDARD_COUNT][DEVICE_ID_OBJECT_MAX + 1];
	/** @brief One extended Read Device Identification object. */
	struct
	{
		u8	id;
		char	value[DEVICE_ID_OBJECT_MAX + 1];
	}		extended_id[DEVICE_MAX_EXTENDED_IDS];
	u32		extended_id_count;
	space_t		spaces[SPACE_COUNT];
	struct machine_system_t* machines; /**< up to MACHINE_MAX_MACHINES, arena-backed */
	struct action_system_t* actions; /**< point actions (on-read/on-write), arena-backed, NULL when none */
	/** @brief Host hook run on Modbus client reads/writes (NULL: none); returns 0 or an exception code. */
	u8		(*access_fn)(void* user, device_t* dev, const device_access_t* access);
	void*		access_user;
	/** @brief Host hook run after a Modbus client write was applied (NULL: none); the access carries the stored values. */
	void		(*written_fn)(void* user, device_t* dev, const device_access_t* access);
	void*		written_user;
	u8		request_unit;		/**< unit id of the request being handled, set by net_handle_pdu() */
};

/**
 * @brief Creates an empty device in the arena.
 *
 * Returns NULL if the arena is out of memory.
 */
device_t*	device_new(arena_t* arena, u64 start_ms);

/**
 * @brief Assigns the device to the server named `id`.
 *
 * Returns NULL on success (also when already assigned), or a static error message.
 */
const char*	device_add_server(device_t* dev, const char* id);

/** @brief True if a server with this id (NULL = a server without id) serves the device. */
bool		device_serves(const device_t* dev, const char* id);

/** @brief True if some server serves both devices, so their unit ids must differ. */
bool		device_shares_server(const device_t* a, const device_t* b);

/**
 * @brief Adds a point.
 *
 * Returns NULL on success, or a static error message.
 */
const char*	device_add_point(device_t* dev, const point_t* point);

/** @brief Registers (or bits) the point occupies: value_width(type), or `length` for a string. */
u32		point_width(const point_t* point);

/** @brief Index of the point whose first address is `address`, or DEVICE_MAX_POINTS. */
u32		device_find_point(const device_t* dev, space_kind_t space, u32 address);

/**
 * @brief Moves every ramp to its value at `now_ms`.
 *
 * Call before serving a read.
 */
void		device_tick(device_t* dev, u64 now_ms);

/**
 * @brief Starts every model from scratch at `now_ms`: lag from its initial value (or goal), stuck
 *        values released.
 *
 * Call once the whole device is built, because a lag may follow a point declared after it.
 * device_restart() calls it too.
 */
void		device_reset_models(device_t* dev, u64 now_ms);

/**
 * @brief True when a read of [address, address+count) covers a point whose degradation has
 *        failed it, at the device time of the last device_tick().
 *
 * Read handlers answer exception 04 for it. The point's configured `fail-after-ms` applies
 * whatever its current kind: a client write or a host claim removes the model effects but not
 * the failure; restart heals it. Writes are not checked.
 */
bool		device_range_failed(const device_t* dev, space_kind_t space, u32 address, u32 count);

/** @brief True for points whose value changes with time: ramps, models and clocks. */
bool		point_is_timed(const point_t* point);

/**
 * @brief Restarts the device at `now_ms`: restores every point to its configured kind and
 *        initial value, restarts ramp timelines, resets machine runtimes (initial state, vars, timers
 *        and on-enter), and re-arms fault schedules from `now_ms` except for the restart kind, which
 *        keeps its progress.
 *
 * Protocol counters, files, FIFOs, the RNG seed and configured data stay unchanged.
 */
void		device_restart(device_t* dev, u64 now_ms);

/**
 * @brief Reads `qty` consecutive addresses.
 *
 * Fails if any address is out of range or undefined.
 */
bool		device_read(const device_t* dev, space_kind_t space, u32 address, u32 qty, u16* out);

/**
 * @brief Writes `qty` consecutive addresses.
 *
 * Fails (and writes nothing) if any address is out of range, undefined or machine-owned. A
 * written ramp point becomes static. On success, runs any matching machine on-write handlers
 * in the same request.
 */
bool		device_write(device_t* dev, space_kind_t space, u32 address, u32 qty, const u16* in);

/** @brief True when no address in the range belongs to a read-only or machine-driven point (client writes). */
bool		device_range_writable(const device_t* dev, space_kind_t space, u32 address, u32 qty);

/**
 * @brief device_write() as a Modbus client: also fails if any point in the range is read-only.
 *
 * Host-side writes (the C API, the HTTP control API) use device_write() and may write them.
 */
bool		device_client_write(device_t* dev, space_kind_t space, u32 address, u32 qty, const u16* in);

/**
 * @brief Reads/writes one decoded point value.
 *
 * Machine-driven points use these instead of raw register access. Both fail on strings.
 */
bool		device_get_point(const device_t* dev, space_kind_t space, u32 address, val_t* out);
bool		device_set_point(device_t* dev, space_kind_t space, u32 address, const val_t* in);

/**
 * @brief device_set_point() with host-write semantics, used by point actions.
 *
 * The point becomes POINT_STATIC like after a client write (a machine-driven point stays
 * machine-driven) and no hook, machine on-write handler or action runs.
 */
bool		device_set_point_host(device_t* dev, space_kind_t space, u32 address,
				      const val_t* in);

/**
 * @brief Reads a string point as text, up to its first NUL byte.
 *
 * Fails if no string point starts at `address` or `size` is smaller than the text plus its NUL.
 */
bool		device_get_string(const device_t* dev, space_kind_t space, u32 address, char* out,
				  size_t size);

/**
 * @brief Replaces a string point's registers with `text`, NUL padded; clients may still write it.
 *
 * Fails if no string point starts at `address` or the text is longer than 2*length bytes.
 */
bool		device_set_string(device_t* dev, space_kind_t space, u32 address, const char* text);

/**
 * @brief Adds `length` consecutive file-record register values starting at `record` in file
 *        `file_no`.
 *
 * Values are copied into the device arena. Returns NULL on success, or a static error message.
 */
const char*	device_add_file_record(device_t* dev, u16 file_no, u16 record, u16 length,
				       const u16* values);

/**
 * @brief Reads `length` consecutive file-record registers starting at `record` in `file_no`.
 *
 * Fails if the file or any requested record is not defined.
 */
bool		device_read_file_records(const device_t* dev, u16 file_no, u16 record, u16 length,
					 u16* out);

/**
 * @brief True if `length` consecutive file-record registers starting at `record` in `file_no`
 *        are defined and writable.
 */
bool		device_can_write_file_records(const device_t* dev, u16 file_no, u16 record,
					      u16 length);

/**
 * @brief Writes `length` consecutive file-record registers starting at `record` in `file_no`.
 *
 * Fails if the file or any requested record is not defined.
 */
bool		device_write_file_records(device_t* dev, u16 file_no, u16 record, u16 length,
					  const u16* in);

/**
 * @brief Adds a FIFO queue identified by `address` with `count` register values.
 *
 * Values are copied into the device arena and returned in order by FC 24. Returns NULL on
 * success, or a static error message.
 */
/**
 * @brief Adds a user-defined function code (copied).
 *
 * The caller checks that the code is not one mogbus implements. Returns NULL on success, or a
 * static error message (duplicate code, too many codes, out of memory).
 */
const char*	device_add_custom_fc(device_t* dev, const device_custom_fc_t* fc);

/** @brief The user-defined function code `code`, or NULL. */
const device_custom_fc_t* device_find_custom_fc(const device_t* dev, u8 code);

const char*	device_add_fifo(device_t* dev, u16 address, u16 count, const u16* values);

/**
 * @brief Reads a configured FIFO queue identified by `address`.
 *
 * On success sets `*count` (0..DEVICE_MAX_FIFO_VALUES) and copies values into `out`. Fails if
 * the FIFO is not defined.
 */
bool		device_read_fifo(const device_t* dev, u16 address, u16* count, u16* out);

/**
 * @brief Replaces the contents of the FIFO queue at `address` with `count` values.
 *
 * Fails if the FIFO is not defined or `count` is above DEVICE_MAX_FIFO_VALUES.
 */
bool		device_set_fifo(device_t* dev, u16 address, u16 count, const u16* values);

/**
 * @brief Adds a CANopen object.
 *
 * Returns NULL on success, or a static error message. Node ids are 1..127, length is
 * 1..DEVICE_MAX_CANOPEN_OBJECT_BYTES, and each (node, index, subindex) triple may be
 * configured only once per device.
 */
const char*	device_add_canopen_object(device_t* dev, u8 node_id, u16 index, u8 subindex,
					  u16 length, const u8* data);

/** @brief Finds a configured CANopen object, or NULL if there is none. */
const device_canopen_object_t* device_find_canopen_object(const device_t* dev, u8 node_id,
							   u16 index, u8 subindex);

/**
 * @brief Reads/writes `count` bytes at `offset` in a configured object.
 *
 * Fails if the object is absent or the range is outside its stored data. All configured
 * objects are writable.
 */
bool		device_canopen_read(const device_t* dev, u8 node_id, u16 index, u8 subindex,
				    u16 offset, u16 count, u8* out);
bool		device_canopen_write(device_t* dev, u8 node_id, u16 index, u8 subindex,
				     u16 offset, u16 count, const u8* in);

/** @brief True for SPACE_COIL and SPACE_DISCRETE. */
bool		space_is_bit(space_kind_t space);

const char*	space_name(space_kind_t space);

#endif
