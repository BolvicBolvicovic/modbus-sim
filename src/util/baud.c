#include "util/baud.h"

typedef struct baud_entry_t baud_entry_t;
struct baud_entry_t
{
	u32	baud;
	speed_t	speed;
};

#define BAUD_ENTRY(rate) { rate, B##rate }

static const baud_entry_t g_baud_table[] = {
#ifdef B50
	BAUD_ENTRY(50),
#endif
#ifdef B75
	BAUD_ENTRY(75),
#endif
#ifdef B110
	BAUD_ENTRY(110),
#endif
#ifdef B134
	BAUD_ENTRY(134),
#endif
#ifdef B150
	BAUD_ENTRY(150),
#endif
#ifdef B200
	BAUD_ENTRY(200),
#endif
#ifdef B300
	BAUD_ENTRY(300),
#endif
#ifdef B600
	BAUD_ENTRY(600),
#endif
#ifdef B1200
	BAUD_ENTRY(1200),
#endif
#ifdef B1800
	BAUD_ENTRY(1800),
#endif
#ifdef B2400
	BAUD_ENTRY(2400),
#endif
#ifdef B4800
	BAUD_ENTRY(4800),
#endif
#ifdef B9600
	BAUD_ENTRY(9600),
#endif
#ifdef B19200
	BAUD_ENTRY(19200),
#endif
#ifdef B38400
	BAUD_ENTRY(38400),
#endif
#ifdef B57600
	BAUD_ENTRY(57600),
#endif
#ifdef B115200
	BAUD_ENTRY(115200),
#endif
#ifdef B230400
	BAUD_ENTRY(230400),
#endif
#ifdef B460800
	BAUD_ENTRY(460800),
#endif
#ifdef B500000
	BAUD_ENTRY(500000),
#endif
#ifdef B576000
	BAUD_ENTRY(576000),
#endif
#ifdef B921600
	BAUD_ENTRY(921600),
#endif
#ifdef B1000000
	BAUD_ENTRY(1000000),
#endif
#ifdef B1152000
	BAUD_ENTRY(1152000),
#endif
#ifdef B1500000
	BAUD_ENTRY(1500000),
#endif
#ifdef B2000000
	BAUD_ENTRY(2000000),
#endif
#ifdef B2500000
	BAUD_ENTRY(2500000),
#endif
#ifdef B3000000
	BAUD_ENTRY(3000000),
#endif
#ifdef B3500000
	BAUD_ENTRY(3500000),
#endif
#ifdef B4000000
	BAUD_ENTRY(4000000),
#endif
};

bool
baud_to_speed(u32 baud, speed_t* out)
{
	for (u32 i = 0; i < (u32)(sizeof(g_baud_table) / sizeof(g_baud_table[0])); i++)
	{
		if (g_baud_table[i].baud != baud)
			continue;
		*out = g_baud_table[i].speed;
		return true;
	}
	return false;
}

bool
baud_is_standard(u32 baud)
{
	speed_t	speed;

	return baud_to_speed(baud, &speed);
}

u32
baud_from_speed(speed_t speed)
{
	for (u32 i = 0; i < (u32)(sizeof(g_baud_table) / sizeof(g_baud_table[0])); i++)
	{
		if (g_baud_table[i].speed == speed)
			return g_baud_table[i].baud;
	}
	return 0;
}
