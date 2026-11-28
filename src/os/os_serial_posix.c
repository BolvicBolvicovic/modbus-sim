/**
 * @file
 * @brief POSIX serial devices and PTYs: termios line setup, baud table, raw byte-stream I/O.
 */
#include "os/os_serial.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/serial.h>
#include <pty.h>
#endif

_Static_assert(sizeof(tcflag_t) == sizeof(u32), "tcflag_t is not 32 bits");

typedef struct baud_entry_t baud_entry_t;

/** @brief A baud rate and its termios speed constant. */
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

static bool
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
os_serial_supported(void)
{
	return true;
}

bool
os_baud_is_standard(u32 baud)
{
	speed_t	speed;

	return baud_to_speed(baud, &speed);
}

static u32
baud_from_speed(speed_t speed)
{
	for (u32 i = 0; i < (u32)(sizeof(g_baud_table) / sizeof(g_baud_table[0])); i++)
	{
		if (g_baud_table[i].speed == speed)
			return g_baud_table[i].baud;
	}
	return 0;
}

bool
os_serial_line_flags(u32 data_bits, parity_t parity, u32 stop_bits, u32* clear, u32* set)
{
	*clear = CSIZE | PARENB | PARODD | CSTOPB;
	*set   = data_bits == 7 ? CS7 : CS8;
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

static bool
tty_is_pty(os_serial_t fd)
{
	char		name[64];
#if defined(__linux__)
	unsigned int	ptn = 0;
#endif

	if (!isatty(fd))
		return false;
	if (ttyname_r(fd, name, sizeof(name)) == 0)
	{
		if (strcmp(name, "/dev/ptmx") == 0)
			return true;
		if (strncmp(name, "/dev/pts/", 9) == 0)
			return true;
	}
#if defined(__linux__)
	/* Fallback for master fds whose ttyname_r() may be non-PTS. */
	if (ioctl(fd, TIOCGPTN, &ptn) == 0)
		return true;
#endif
	return false;
}

u32
os_serial_baud(os_serial_t fd)
{
	struct termios	tio;

	if (!isatty(fd) || tty_is_pty(fd))
		return 0;
	if (tcgetattr(fd, &tio) != 0)
		return 0;
	return baud_from_speed(cfgetospeed(&tio));
}

/**
 * @brief Applies raw mode with the selected line format.
 *
 * When baud is 0, preserves the existing baud rate so a configured serial line keeps its
 * speed. Non-tty fds are skipped.
 */
static bool
set_tty_raw(os_serial_t fd, u32 baud, u32 data_bits, parity_t parity, u32 stop_bits)
{
	struct termios	tio;
	speed_t		ispeed;
	speed_t		ospeed;
	speed_t		requested = B9600;
	u32		clear = 0;
	u32		set = 0;

	if (!isatty(fd))
		return true;
	if (!os_serial_line_flags(data_bits, parity, stop_bits, &clear, &set))
	{
		errno = EOPNOTSUPP;
		return false;
	}
	if (tcgetattr(fd, &tio) != 0)
		return false;
	if (baud > 0 && !baud_to_speed(baud, &requested))
	{
		errno = EINVAL;
		return false;
	}

	ispeed = cfgetispeed(&tio);
	ospeed = cfgetospeed(&tio);
	cfmakeraw(&tio);
	if (baud > 0)
	{
		ispeed = requested;
		ospeed = requested;
	}
	cfsetispeed(&tio, ispeed);
	cfsetospeed(&tio, ospeed);
	tio.c_cflag &= ~(clear | CRTSCTS);
	tio.c_cflag |= set | CLOCAL | CREAD;
	tio.c_iflag &= ~(IXON | IXOFF | IXANY);
#ifdef ECHOE
	tio.c_lflag &= ~ECHOE;
#endif
#ifdef ECHOK
	tio.c_lflag &= ~ECHOK;
#endif
#ifdef ECHONL
	tio.c_lflag &= ~ECHONL;
#endif
#ifdef ECHOCTL
	tio.c_lflag &= ~ECHOCTL;
#endif
#ifdef ECHOKE
	tio.c_lflag &= ~ECHOKE;
#endif
#ifdef ECHOPRT
	tio.c_lflag &= ~ECHOPRT;
#endif
#ifdef NOKERNINFO
	tio.c_lflag &= ~NOKERNINFO;
#endif
	if (tcsetattr(fd, TCSANOW, &tio) != 0)
		return false;
	return true;
}

os_serial_t
os_serial_open(bool pty, const char* label, const char* path, u32 baud,
	       u32 data_bits, parity_t parity, u32 stop_bits,
	       os_serial_t* slave_fd, char* name, size_t name_size)
{
#if defined(__linux__)
	char	pty_name[128];
#endif
	int	fd       = -1;
	int	slave    = -1;
	int	flags;

	*slave_fd = -1;
	if (pty)
	{
#if defined(__linux__)
		if (openpty(&fd, &slave, pty_name, NULL, NULL) < 0)
		{
			fprintf(stderr, "cannot create PTY: %s\n", strerror(errno));
			return -1;
		}
		if (!set_tty_raw(slave, baud, data_bits, parity, stop_bits))
		{
			fprintf(stderr, "cannot configure %s PTY slave: %s\n", label, strerror(errno));
			close(slave);
			close(fd);
			return -1;
		}
		if (name && name_size > 0)
			snprintf(name, name_size, "%s", pty_name);
		*slave_fd = slave;
#else
		fprintf(stderr, "%s PTY mode is not supported on this platform\n", label);
		errno = EOPNOTSUPP;
		return -1;
#endif
	}
	else
	{
		fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
		if (fd < 0)
		{
			fprintf(stderr, "cannot open %s device '%s': %s\n",
				label, path, strerror(errno));
			return -1;
		}
	}

	if (!set_tty_raw(fd, baud, data_bits, parity, stop_bits))
	{
		fprintf(stderr, "cannot configure %s device '%s': %s\n",
			label, pty ? "pty" : path, strerror(errno));
		goto fail;
	}
	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
	{
		fprintf(stderr, "cannot set %s fd non-blocking: %s\n", label, strerror(errno));
		goto fail;
	}
	return fd;

fail:
	if (slave >= 0)
		close(slave);
	close(fd);
	*slave_fd = -1;
	return -1;
}

bool
os_serial_low_latency(os_serial_t fd)
{
#if defined(__linux__) && defined(TIOCGSERIAL) && defined(ASYNC_LOW_LATENCY)
	struct serial_struct	ss;

	if (!isatty(fd) ||
	    ioctl(fd, TIOCGSERIAL, &ss) != 0)
		return false;
	if (ss.flags & ASYNC_LOW_LATENCY)
		return true;
	ss.flags |= ASYNC_LOW_LATENCY;
	return ioctl(fd, TIOCSSERIAL, &ss) == 0;
#else
	(void)fd;
	return false;
#endif
}

s64
os_serial_read(os_serial_t fd, void* buf, u32 len)
{
	return read(fd, buf, len);
}

s64
os_serial_write(os_serial_t fd, const void* buf, u32 len)
{
	return write(fd, buf, len);
}

void
os_serial_close(os_serial_t fd)
{
	close(fd);
}
