#include "util/baud.h"

bool
baud_to_speed(u32 baud, speed_t* out)
{
	switch (baud)
	{
#ifdef B50
	case 50: *out = B50; return true;
#endif
#ifdef B75
	case 75: *out = B75; return true;
#endif
#ifdef B110
	case 110: *out = B110; return true;
#endif
#ifdef B134
	case 134: *out = B134; return true;
#endif
#ifdef B150
	case 150: *out = B150; return true;
#endif
#ifdef B200
	case 200: *out = B200; return true;
#endif
#ifdef B300
	case 300: *out = B300; return true;
#endif
#ifdef B600
	case 600: *out = B600; return true;
#endif
#ifdef B1200
	case 1200: *out = B1200; return true;
#endif
#ifdef B1800
	case 1800: *out = B1800; return true;
#endif
#ifdef B2400
	case 2400: *out = B2400; return true;
#endif
#ifdef B4800
	case 4800: *out = B4800; return true;
#endif
#ifdef B9600
	case 9600: *out = B9600; return true;
#endif
#ifdef B19200
	case 19200: *out = B19200; return true;
#endif
#ifdef B38400
	case 38400: *out = B38400; return true;
#endif
#ifdef B57600
	case 57600: *out = B57600; return true;
#endif
#ifdef B115200
	case 115200: *out = B115200; return true;
#endif
#ifdef B230400
	case 230400: *out = B230400; return true;
#endif
#ifdef B460800
	case 460800: *out = B460800; return true;
#endif
#ifdef B500000
	case 500000: *out = B500000; return true;
#endif
#ifdef B576000
	case 576000: *out = B576000; return true;
#endif
#ifdef B921600
	case 921600: *out = B921600; return true;
#endif
#ifdef B1000000
	case 1000000: *out = B1000000; return true;
#endif
#ifdef B1152000
	case 1152000: *out = B1152000; return true;
#endif
#ifdef B1500000
	case 1500000: *out = B1500000; return true;
#endif
#ifdef B2000000
	case 2000000: *out = B2000000; return true;
#endif
#ifdef B2500000
	case 2500000: *out = B2500000; return true;
#endif
#ifdef B3000000
	case 3000000: *out = B3000000; return true;
#endif
#ifdef B3500000
	case 3500000: *out = B3500000; return true;
#endif
#ifdef B4000000
	case 4000000: *out = B4000000; return true;
#endif
	default: return false;
	}
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
	switch (speed)
	{
#ifdef B50
	case B50: return 50;
#endif
#ifdef B75
	case B75: return 75;
#endif
#ifdef B110
	case B110: return 110;
#endif
#ifdef B134
	case B134: return 134;
#endif
#ifdef B150
	case B150: return 150;
#endif
#ifdef B200
	case B200: return 200;
#endif
#ifdef B300
	case B300: return 300;
#endif
#ifdef B600
	case B600: return 600;
#endif
#ifdef B1200
	case B1200: return 1200;
#endif
#ifdef B1800
	case B1800: return 1800;
#endif
#ifdef B2400
	case B2400: return 2400;
#endif
#ifdef B4800
	case B4800: return 4800;
#endif
#ifdef B9600
	case B9600: return 9600;
#endif
#ifdef B19200
	case B19200: return 19200;
#endif
#ifdef B38400
	case B38400: return 38400;
#endif
#ifdef B57600
	case B57600: return 57600;
#endif
#ifdef B115200
	case B115200: return 115200;
#endif
#ifdef B230400
	case B230400: return 230400;
#endif
#ifdef B460800
	case B460800: return 460800;
#endif
#ifdef B500000
	case B500000: return 500000;
#endif
#ifdef B576000
	case B576000: return 576000;
#endif
#ifdef B921600
	case B921600: return 921600;
#endif
#ifdef B1000000
	case B1000000: return 1000000;
#endif
#ifdef B1152000
	case B1152000: return 1152000;
#endif
#ifdef B1500000
	case B1500000: return 1500000;
#endif
#ifdef B2000000
	case B2000000: return 2000000;
#endif
#ifdef B2500000
	case B2500000: return 2500000;
#endif
#ifdef B3000000
	case B3000000: return 3000000;
#endif
#ifdef B3500000
	case B3500000: return 3500000;
#endif
#ifdef B4000000
	case B4000000: return 4000000;
#endif
	default: return 0;
	}
}
