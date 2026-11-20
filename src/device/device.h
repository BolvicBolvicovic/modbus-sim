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

/* Name: device_t
 * Description: one simulated Modbus device (one unit id).
 * */
typedef struct device_t device_t;
struct device_t
{
	char		name[DEVICE_NAME_MAX];
	u8		unit_id;
	u64		start_ms;	/* ramps count their time from here */
	fault_t		fault;
	u32		point_count;
	point_t*	points;		/* DEVICE_MAX_POINTS entries */
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

/* True for SPACE_COIL and SPACE_DISCRETE. */
bool		space_is_bit(space_kind_t space);

const char*	space_name(space_kind_t space);

#endif
