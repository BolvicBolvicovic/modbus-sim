#include "util/serial.h"
#include "util/baud.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#if defined(__linux__)
#include <pty.h>
#include <sys/ioctl.h>
#endif

bool
serial_line_flags(u32 data_bits, parity_t parity, u32 stop_bits,
		  tcflag_t* clear, tcflag_t* set)
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
tty_is_pty(int fd)
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
serial_tty_baud(int fd)
{
	struct termios	tio;

	if (!isatty(fd) || tty_is_pty(fd))
		return 0;
	if (tcgetattr(fd, &tio) != 0)
		return 0;
	return baud_from_speed(cfgetospeed(&tio));
}

/* Applies raw mode with the selected line format. When baud is 0, preserves
 * the existing baud rate so a configured serial line keeps its speed.
 * Non-tty fds are skipped. */
static bool
set_tty_raw(int fd, u32 baud, u32 data_bits, parity_t parity, u32 stop_bits)
{
	struct termios	tio;
	speed_t		ispeed;
	speed_t		ospeed;
	speed_t		requested = B9600;
	tcflag_t	clear = 0;
	tcflag_t	set = 0;

	if (!isatty(fd))
		return true;
	if (!serial_line_flags(data_bits, parity, stop_bits, &clear, &set))
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

int
open_serial_fd(bool pty, const char* label, const char* path, u32 baud,
	       u32 data_bits, parity_t parity, u32 stop_bits,
	       int* slave_fd, char* name, size_t name_size)
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
