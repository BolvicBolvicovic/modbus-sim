#include "util/serial.h"

bool
serial_line_flags(parity_t parity, u32 stop_bits, tcflag_t* clear, tcflag_t* set)
{
	*clear = CSIZE | PARENB | PARODD | CSTOPB;
	*set   = CS8;
#ifdef CMSPAR
	*clear |= CMSPAR;
#else
	if (parity == PARITY_MARK || parity == PARITY_SPACE)
		return false;
#endif

	if (parity != PARITY_NONE)
	{
		*set |= PARENB;
		if (parity == PARITY_ODD || parity == PARITY_MARK)
			*set |= PARODD;
#ifdef CMSPAR
		if (parity == PARITY_MARK || parity == PARITY_SPACE)
			*set |= CMSPAR;
#endif
	}
	if (stop_bits == 2)
		*set |= CSTOPB;
	return true;
}
