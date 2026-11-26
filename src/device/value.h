/**
 * @file
 * @brief Values used by scripted device machines.
 */
#ifndef VALUE_H
#define VALUE_H

#include "vlib/c_types.h"
#include <stdbool.h>

typedef enum val_type_t val_type_t;
typedef struct val_t val_t;

/** @brief The only two value types used by scripted device machines. */
enum val_type_t
{
	VAL_BOOL,
	VAL_NUM,
};

/** @brief A machine value: a number or a boolean. */
struct val_t
{
	val_type_t	type;
	bool		b;
	f64		n;
};

#endif
