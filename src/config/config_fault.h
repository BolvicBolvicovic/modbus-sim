#ifndef CONFIG_FAULT_H
#define CONFIG_FAULT_H

#include "config/form.h"
#include "device/fault.h"

/* Parses one (fault ...) form into `set`, using the same rules for device-level
 * faults and machine state overlays. `owner` describes the enclosing form in
 * messages, for example "device" or "machine 'm', state 'idle'". */
bool	config_parse_fault(form_ctx_t* ctx, s7_pointer item, fault_set_t* set,
			   const char* owner);

#endif
