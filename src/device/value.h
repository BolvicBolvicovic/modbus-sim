/**
 * @file
 * @brief Values used by scripted device machines.
 */
#ifndef VALUE_H
#define VALUE_H

#include "vlib/c_types.h"
#include <stdbool.h>

/** @brief The only two value types used by scripted device machines. */
typedef enum val_type_t val_type_t;
enum val_type_t
{
	VAL_BOOL,
	VAL_NUM,
};

typedef struct val_t val_t;
struct val_t
{
	val_type_t	type;
	bool		b;
	f64		n;
};

#endif
