/**
 * @file
 * @brief Windows serial stub: serial ports, PTYs and byte-stream handles are not implemented yet.
 *
 * The baud table is the POSIX one so a config validates the same on every platform.
 */
#include "os/os_serial.h"
#include <stdio.h>

static const u32 g_baud_table[] = {
	50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400, 4800, 9600, 19200, 38400, 57600,
	115200, 230400, 460800, 500000, 576000, 921600, 1000000, 1152000, 1500000, 2000000,
	2500000, 3000000, 3500000, 4000000,
};

bool
os_serial_supported(void)
{
	return false;
}

bool
os_baud_is_standard(u32 baud)
{
	for (u32 i = 0; i < (u32)(sizeof(g_baud_table) / sizeof(g_baud_table[0])); i++)
	{
		if (g_baud_table[i] == baud)
			return true;
	}
	return false;
}

u32
os_serial_baud(os_serial_t fd)
{
	(void)fd;
	return 0;
}

os_serial_t
os_serial_open(bool pty, const char* label, const char* path, u32 baud,
	       u32 data_bits, parity_t parity, u32 stop_bits,
	       os_serial_t* slave_fd, char* name, size_t name_size)
{
	(void)path;
	(void)baud;
	(void)data_bits;
	(void)parity;
	(void)stop_bits;
	(void)slave_fd;
	(void)name;
	(void)name_size;
	if (pty)
		fprintf(stderr, "%s PTY mode is not available on Windows\n", label);
	else
		fprintf(stderr, "%s serial ports are not supported on Windows yet\n", label);
	return OS_SERIAL_INVALID;
}

s64
os_serial_read(os_serial_t fd, void* buf, u32 len)
{
	(void)fd;
	(void)buf;
	(void)len;
	return -1;
}

s64
os_serial_write(os_serial_t fd, const void* buf, u32 len)
{
	(void)fd;
	(void)buf;
	(void)len;
	return -1;
}

void
os_serial_close(os_serial_t fd)
{
	(void)fd;
}
