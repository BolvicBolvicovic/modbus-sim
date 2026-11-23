#ifndef DEVICE_H
#define DEVICE_H

#include "vlib/c_types.h"
#include "vlib/arena.h"
#include "device/behaviour.h"
#include "device/encode.h"
#include "device/fault.h"
#include <stdbool.h>
#include <stddef.h>

#define DEVICE_NAME_MAX		64
#define DEVICE_ID_OBJECT_MAX	244	/* 7-byte MEI header + 2-byte object header + value <= MODBUS_MAX_PDU */
/* 253-byte Modbus PDU minus FC, byte count, server ID and run indicator. */
#define DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX	249
#define DEVICE_MAX_EXTENDED_IDS	32
#define DEVICE_MAX_POINTS	8192
#define ADDRESS_SPACE_SIZE	65536

/* Name: space_kind_t
 * Description: the four Modbus data tables.
 * */
typedef enum space_kind_t space_kind_t;
enum space_kind_t
{
	SPACE_COIL,	/* read/write bit    (FC 01, 05, 15) */
	SPACE_DISCRETE,	/* read-only bit     (FC 02)         */
	SPACE_INPUT,	/* read-only 16 bits (FC 04)         */
	SPACE_HOLDING,	/* read/write 16 bits(FC 03, 06, 16) */
	SPACE_COUNT,
};

typedef enum point_kind_t point_kind_t;
enum point_kind_t
{
	POINT_STATIC,	/* value never changes by itself */
	POINT_RAMP,	/* value follows a ramp over time */
};

/* Name: point_t
 * Description: one configured data point: where it lives, how it is encoded,
 * and how its value behaves. A point occupies value_width(type) addresses.
 * */
typedef struct point_t point_t;
struct point_t
{
	space_kind_t	space;
	u32		address;
	value_type_t	type;
	word_order_t	order;
	point_kind_t	kind;
	f64		static_value;
	ramp_params_t	ramp;
};

/* Name: space_t
 * Description: one data table. `owner` is the 1-based index of the point that
 * defines each address (0 = address not defined). Bit spaces store 0 or 1.
 * */
typedef struct space_t space_t;
struct space_t
{
	u16	value[ADDRESS_SPACE_SIZE];
	u16	owner[ADDRESS_SPACE_SIZE];
};

#define DEVICE_MAX_FILES		16
#define DEVICE_MAX_FILE_SEGMENTS	64
#define DEVICE_MAX_RECORD_VALUES	126
#define DEVICE_MAX_FIFOS		16
#define DEVICE_MAX_FIFO_VALUES		31
#define DEVICE_MAX_COM_EVENTS		64

/* Name: file_segment_t
 * Description: one contiguous block of 16-bit register values in a file,
 * starting at Modbus record number `record`.
 * */
typedef struct file_segment_t file_segment_t;
struct file_segment_t
{
	u16	record;
	u16	length;
	u16*	values;	/* length entries, arena-backed */
};

/* Name: device_file_t
 * Description: configured FC 20 (Read File Record) storage for one file number.
 * */
typedef struct device_file_t device_file_t;
struct device_file_t
{
	u16		file_no;
	u32		segment_count;
	file_segment_t	segments[DEVICE_MAX_FILE_SEGMENTS];
};

/* Name: device_fifo_t
 * Description: configured FC 24 (Read FIFO Queue) storage for one FIFO pointer.
 * Values are returned in configured order, non-destructively.
 * */
typedef struct device_fifo_t device_fifo_t;
struct device_fifo_t
{
	u16	address;	/* FIFO pointer address */
	u16	count;		/* 0..DEVICE_MAX_FIFO_VALUES */
	u16*	values;		/* count entries, arena-backed */
};

/* FC 08 diagnostic register bits. The simulator has no UART character stream,
 * so the character-overrun flag/count are always clear; 0x0014 still resets them. */
enum diagnostic_register_bit
{
	DIAG_REG_CHARACTER_OVERRUN       = 1u << 0,
	DIAG_REG_ASCII_DELIMITER_CHANGED = 1u << 5,
};

/* MEI 14 (Read Device Identification) object ids. */
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

/* Name: device_t
 * Description: one simulated Modbus device (one unit id).
 * */
typedef struct device_t device_t;
struct device_t
{
	char		name[DEVICE_NAME_MAX];
	u8		unit_id;
	u8		exception_status;	/* FC 07 (Read Exception Status) response byte */
	u8		server_id;		/* FC 17 (Report Server ID) server ID byte */
	u8		run_indicator;		/* FC 17 Run Indicator Status (0xFF run, 0x00 off) */
	u8		server_id_additional_data[DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX];
	u8		server_id_additional_data_len;
	u16		com_status;		/* FC 11/12 status: 0 ready, 0xFFFF busy */
	u16		com_event_count;	/* FC 11/12 event counter, wraps modulo 65536 */
	u16		message_count;		/* FC 12 + FC 08 bus/slave message count */
	u16		com_event_log_count;	/* number of configured FC 12 event bytes */
	u8		com_events[DEVICE_MAX_COM_EVENTS]; /* FC 12 event bytes, in order */
	u16		diagnostic_register;	/* FC 08 subfunction 0x0002 bitmask */
	u16		diag_comm_error_count;	/* FC 08 subfunction 0x000C */
	u16		diag_exception_count;	/* FC 08 subfunction 0x000D */
	u16		diag_no_response_count;	/* FC 08 subfunction 0x000F */
	u16		diag_nak_count;		/* FC 08 subfunction 0x0010 */
	u16		diag_busy_count;	/* FC 08 subfunction 0x0011 */
	u16		diag_overrun_count;	/* FC 08 subfunction 0x0012 */
	u8		ascii_delimiter;	/* FC 08 subfunction 0x0003, default LF */
	bool		listen_only;		/* FC 08 subfunction 0x0004 */
	u64		start_ms;		/* ramps count their time from here */
	fault_t		fault;
	arena_t*	arena;			/* backing arena for dynamic config storage */
	u32		point_count;
	point_t*	points;			/* DEVICE_MAX_POINTS entries */
	u32		file_count;
	device_file_t*	files;			/* DEVICE_MAX_FILES entries */
	u32		fifo_count;
	device_fifo_t*	fifos;			/* DEVICE_MAX_FIFOS entries */
	char		device_id[DEVICE_ID_STANDARD_COUNT][DEVICE_ID_OBJECT_MAX + 1];
	struct
	{
		u8	id;
		char	value[DEVICE_ID_OBJECT_MAX + 1];
	}		extended_id[DEVICE_MAX_EXTENDED_IDS];
	u32		extended_id_count;
	space_t		spaces[SPACE_COUNT];
};

/* Creates an empty device in the arena. Returns NULL if the arena is out of memory. */
device_t*	device_new(arena_t* arena, u64 start_ms);

/* Adds a point. Returns NULL on success, or a static error message. */
const char*	device_add_point(device_t* dev, const point_t* point);

/* Moves every ramp to its value at `now_ms`. Call before serving a read. */
void		device_tick(device_t* dev, u64 now_ms);

/* Reads `qty` consecutive addresses. Fails if any address is out of range or undefined. */
bool		device_read(const device_t* dev, space_kind_t space, u32 address, u32 qty, u16* out);

/* Writes `qty` consecutive addresses. Fails (and writes nothing) if any address is
 * out of range or undefined. A written ramp point becomes static. */
bool		device_write(device_t* dev, space_kind_t space, u32 address, u32 qty, const u16* in);

/* Adds `length` consecutive file-record register values starting at `record`
 * in file `file_no`. Values are copied into the device arena. Returns NULL on
 * success, or a static error message. */
const char*	device_add_file_record(device_t* dev, u16 file_no, u16 record, u16 length,
				       const u16* values);

/* Reads `length` consecutive file-record registers starting at `record` in
 * `file_no`. Fails if the file or any requested record is not defined. */
bool		device_read_file_records(const device_t* dev, u16 file_no, u16 record, u16 length,
					 u16* out);

/* True if `length` consecutive file-record registers starting at `record` in
 * `file_no` are defined and writable. */
bool		device_can_write_file_records(const device_t* dev, u16 file_no, u16 record,
					      u16 length);

/* Writes `length` consecutive file-record registers starting at `record` in
 * `file_no`. Fails if the file or any requested record is not defined. */
bool		device_write_file_records(device_t* dev, u16 file_no, u16 record, u16 length,
					  const u16* in);

/* Adds a FIFO queue identified by `address` with `count` register values.
 * Values are copied into the device arena and returned in order by FC 24.
 * Returns NULL on success, or a static error message. */
const char*	device_add_fifo(device_t* dev, u16 address, u16 count, const u16* values);

/* Reads a configured FIFO queue identified by `address`. On success sets
 * `*count` (0..DEVICE_MAX_FIFO_VALUES) and copies values into `out`.
 * Fails if the FIFO is not defined. */
bool		device_read_fifo(const device_t* dev, u16 address, u16* count, u16* out);

/* True for SPACE_COIL and SPACE_DISCRETE. */
bool		space_is_bit(space_kind_t space);

const char*	space_name(space_kind_t space);

#endif
