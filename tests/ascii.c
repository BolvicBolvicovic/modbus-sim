#define _GNU_SOURCE
#include "vtest.h"
#include "modbus/ascii.h"
#include "modbus/bytes.h"
#include "modbus/pdu.h"
#include "net/ascii_server.h"
#include "net/net_common.h"
#include "device/device.h"
#include "util/clock.h"
#include "vlib/arena.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

TEST(AsciiLrcKnownVector)
{
	/* Modbus ASCII spec example: 01 03 00 00 00 0A -> LRC F2. */
	const u8 frame[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A };

	ASSERT_EQ(modbus_ascii_lrc(frame, sizeof(frame)) == 0xF2);
}

TEST(AsciiLrcIsTwosComplementSum)
{
	const u8 frame[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x01 };

	ASSERT_EQ(modbus_ascii_lrc(frame, sizeof(frame)) == 0xFB);
	ASSERT_EQ(modbus_ascii_lrc(frame, 0) == 0);
}

TEST(AsciiEncodeBuildsUppercaseFrame)
{
	const u8 pdu[] = { 0x03, 0x00, 0x00, 0x00, 0x01 };
	const char expected[] = ":010300000001FB\r\n";
	u8 buf[MODBUS_ASCII_MAX_FRAME];
	u32 len = modbus_ascii_encode(buf, 0x01, pdu, sizeof(pdu));

	ASSERT_EQ(len == sizeof(expected) - 1);
	ASSERT_EQ(memcmp(buf, expected, len) == 0);
}

TEST(AsciiEncodeRejectsEmptyAndOversizedPdu)
{
	u8 pdu[MODBUS_MAX_PDU + 1];
	u8 buf[MODBUS_ASCII_MAX_FRAME + 4];

	memset(pdu, 0, sizeof(pdu));
	ASSERT_EQ(modbus_ascii_encode(buf, 1, pdu, 0) == 0);
	ASSERT_EQ(modbus_ascii_encode(buf, 1, pdu, sizeof(pdu)) == 0);
}

TEST(AsciiPendingResponseBufferHoldsMaximumFrame)
{
	pending_response_t pending = { 0 };
	u8		pdu[MODBUS_MAX_PDU];
	u8		decoded[MODBUS_MAX_PDU + 2];
	const u8*	out = NULL;
	u8		addr = 0;
	u32		pdu_len = 0;
	u32		len = 0;

	/* ASCII hex encoding makes this frame about twice the RTU maximum. */
	ASSERT_EQ(sizeof(pending.buf) >= MODBUS_ASCII_MAX_FRAME);
	for (u32 i = 0; i < sizeof(pdu); i++)
		pdu[i] = (u8)(i * 7 + 0x31);
	len = modbus_ascii_encode(pending.buf, 0x5A, pdu, sizeof(pdu));
	ASSERT_EQ(len == MODBUS_ASCII_MAX_FRAME);
	ASSERT_EQ(modbus_ascii_decode(pending.buf, len, decoded, &addr, &out, &pdu_len));
	ASSERT_EQ(addr == 0x5A);
	ASSERT_EQ(pdu_len == MODBUS_MAX_PDU);
	ASSERT_EQ(memcmp(out, pdu, sizeof(pdu)) == 0);

	pending.active = true;
	pending.len    = len;
	pending.sent   = 11;
	pending.due_ms = 1234;
	pending_response_clear(&pending);
	ASSERT_EQ(!pending.active && pending.len == 0 && pending.sent == 0 && pending.due_ms == 0);
}

TEST(AsciiDecodeRoundTrip)
{
	const u8 pdu[] = { 0x10, 0x00, 0x01, 0x00, 0x02, 0x04, 0x00, 0x0A, 0x01, 0x02 };
	u8 buf[MODBUS_ASCII_MAX_FRAME];
	u8 decoded[MODBUS_MAX_PDU + 2];
	u8 addr = 0;
	const u8* out = NULL;
	u32 out_len = 0;
	u32 frame_len = modbus_ascii_encode(buf, 0x11, pdu, sizeof(pdu));

	ASSERT_SUCCESS(modbus_ascii_decode(buf, frame_len, decoded, &addr, &out, &out_len));
	ASSERT_EQ(addr == 0x11);
	ASSERT_EQ(out_len == sizeof(pdu));
	ASSERT_EQ(memcmp(out, pdu, sizeof(pdu)) == 0);
}

TEST(AsciiDecodeAcceptsLowercaseHex)
{
	const char frame[] = ":010300000001fb\r\n";
	u8 decoded[MODBUS_MAX_PDU + 2];
	u8 addr = 0;
	const u8* pdu = NULL;
	u32 pdu_len = 0;

	ASSERT_SUCCESS(modbus_ascii_decode((const u8*)frame, sizeof(frame) - 1,
					   decoded, &addr, &pdu, &pdu_len));
	ASSERT_EQ(addr == 1 && pdu_len == 5);
	ASSERT_EQ(pdu[0] == 0x03 && get_be16(pdu + 3) == 1);
}

TEST(AsciiDecodeRejectsBadLrc)
{
	const char frame[] = ":010300000001FA\r\n";
	u8 decoded[MODBUS_MAX_PDU + 2];
	u8 addr = 0;
	const u8* pdu = NULL;
	u32 pdu_len = 0;

	ASSERT_FAILURE(modbus_ascii_decode((const u8*)frame, sizeof(frame) - 1,
					   decoded, &addr, &pdu, &pdu_len));
}

TEST(AsciiDecodeRejectsMalformedFrames)
{
	struct
	{
		const char*	name;
		const char*	frame;
	} cases[] = {
		{ "no colon",      "010300000001FB\r\n" },
		{ "no crlf",       ":010300000001FB\rX" },
		{ "wrong order",   ":010300000001FB\n\r" },
		{ "odd hex count", ":01030000001FB\r\n" },
		{ "bad hex digit", ":01G300000001FB\r\n" },
		{ "too short",     ":01FF\r\n" },
	};
	u8 decoded[MODBUS_MAX_PDU + 2];
	u8 addr = 0;
	const u8* pdu = NULL;
	u32 pdu_len = 0;

	for (u32 i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		bool ok = modbus_ascii_decode((const u8*)cases[i].frame,
					      (u32)strlen(cases[i].frame),
					      decoded, &addr, &pdu, &pdu_len);

		ASSERT_FAILURE(ok);
	}
}

TEST(AsciiDecodeRejectsOversizedFrame)
{
	u8 frame[MODBUS_ASCII_MAX_FRAME + 1];
	u8 decoded[MODBUS_MAX_PDU + 2];
	u8 addr = 0;
	const u8* pdu = NULL;
	u32 pdu_len = 0;

	memset(frame, '0', sizeof(frame));
	frame[0] = ':';
	frame[sizeof(frame) - 2] = '\r';
	frame[sizeof(frame) - 1] = '\n';
	ASSERT_FAILURE(modbus_ascii_decode(frame, sizeof(frame), decoded,
					  &addr, &pdu, &pdu_len));
}

TEST(AsciiScanFindsCompleteFrameAndPartial)
{
	const char frame[] = ":010300000001FB\r\n";
	u32 len = 0;

	ASSERT_EQ(modbus_ascii_scan((const u8*)frame, sizeof(frame) - 1, &len) ==
		  MODBUS_ASCII_SCAN_FRAME);
	ASSERT_EQ(len == sizeof(frame) - 1);
	ASSERT_EQ(modbus_ascii_scan((const u8*)frame, sizeof(frame) - 3, &len) ==
		  MODBUS_ASCII_SCAN_NONE);
}

TEST(AsciiScanDiscardsGarbageAndBadTerminators)
{
	const char garbage[] = "junk:010300000001FB\r\n";
	const char lone_cr[] = ":0103\r";
	const char lone_lf[] = ":0103\n";
	u32 len = 0;

	ASSERT_EQ(modbus_ascii_scan((const u8*)garbage, sizeof(garbage) - 1, &len) ==
		  MODBUS_ASCII_SCAN_DISCARD);
	ASSERT_EQ(len == 4);
	ASSERT_EQ(modbus_ascii_scan((const u8*)lone_cr, sizeof(lone_cr) - 1, &len) ==
		  MODBUS_ASCII_SCAN_DISCARD);
	ASSERT_EQ(len == sizeof(lone_cr) - 1);
	ASSERT_EQ(modbus_ascii_scan((const u8*)lone_lf, sizeof(lone_lf) - 1, &len) ==
		  MODBUS_ASCII_SCAN_DISCARD);
	ASSERT_EQ(len == sizeof(lone_lf) - 1);
}

TEST(AsciiScanDiscardsOversizedFrameAndResyncs)
{
	u8 oversized[MODBUS_ASCII_MAX_FRAME + 4];
	u8 full[MODBUS_ASCII_MAX_FRAME];
	u32 len = 0;

	memset(oversized, '0', sizeof(oversized));
	oversized[0] = ':';
	oversized[sizeof(oversized) - 2] = '\r';
	oversized[sizeof(oversized) - 1] = '\n';
	ASSERT_EQ(modbus_ascii_scan(oversized, sizeof(oversized), &len) ==
		  MODBUS_ASCII_SCAN_DISCARD);
	ASSERT_EQ(len == sizeof(oversized));

	memset(full, '0', sizeof(full));
	full[0] = ':';
	full[sizeof(full) - 1] = ':';
	ASSERT_EQ(modbus_ascii_scan(full, sizeof(full), &len) ==
		  MODBUS_ASCII_SCAN_DISCARD);
	ASSERT_EQ(len == sizeof(full) - 1);
}

typedef struct ascii_child_t ascii_child_t;
struct ascii_child_t
{
	pid_t	pid;
	int	fd;	/* test side of socketpair */
};

static bool
send_all_fd(int fd, const u8* buf, u32 len)
{
	while (len > 0)
	{
		ssize_t sent = write(fd, buf, len);

		if (sent < 0 && errno == EINTR)
			continue;
		if (sent <= 0)
			return false;
		buf += sent;
		len -= (u32)sent;
	}
	return true;
}

static bool
recv_ascii_resp(int fd, u8* addr, u8* pdu, u32* pdu_len, u32 timeout_ms)
{
	u8		buf[MODBUS_ASCII_MAX_FRAME * 2];
	u32		got = 0;
	u64		deadline = clock_now_ms() + timeout_ms;

	while (got < sizeof(buf))
	{
		u32			len = 0;
		modbus_ascii_scan_t	scan = modbus_ascii_scan(buf, got, &len);

		if (scan == MODBUS_ASCII_SCAN_FRAME)
		{
			u8 decoded[MODBUS_MAX_PDU + 2];
			const u8* out = NULL;

			if (!modbus_ascii_decode(buf, len, decoded, addr, &out, pdu_len))
				return false;
			memcpy(pdu, out, *pdu_len);
			return true;
		}
		if (scan == MODBUS_ASCII_SCAN_DISCARD)
		{
			memmove(buf, buf + len, got - len);
			got -= len;
			continue;
		}
		{
			u64		now = clock_now_ms();
			struct pollfd	pfd = { .fd = fd, .events = POLLIN };
			ssize_t		n;

			if (now >= deadline)
				return false;
			if (poll(&pfd, 1, (int)(deadline - now)) <= 0)
				continue;
			if (pfd.revents & (POLLERR | POLLNVAL))
				return false;
			if (!(pfd.revents & POLLIN))
				continue;
			n = read(fd, buf + got, sizeof(buf) - got);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				return false;
			got += (u32)n;
		}
	}
	return false;
}

static device_t*
ascii_make_device(arena_t* arena, u8 unit, u16 value)
{
	device_t*	dev = device_new(arena, 0);
	point_t		p   = { 0 };

	if (!dev)
		return NULL;
	dev->unit_id = unit;
	strcpy(dev->name, "ascii-test");
	p.space = SPACE_HOLDING;
	p.address = 0;
	p.type = VT_U16;
	p.kind = POINT_STATIC;
	p.static_value = (f64)value;
	if (device_add_point(dev, &p) != NULL)
		return NULL;
	return dev;
}

static bool
ascii_child_start(ascii_child_t* child, device_t** devices, u32 device_count)
{
	int fds[2] = { -1, -1 };

	child->pid = -1;
	child->fd  = -1;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0)
		return false;

	child->pid = fork();
	if (child->pid < 0)
	{
		close(fds[0]);
		close(fds[1]);
		return false;
	}
	if (child->pid == 0)
	{
		ascii_server_options_t opts = {
			.fd           = fds[1],
			.seed         = 1,
			.devices      = devices,
			.device_count = device_count,
		};

		close(fds[0]);
		(void)ascii_server_run(&opts);
		close(fds[1]);
		_exit(0);
	}

	close(fds[1]);
	child->fd = fds[0];
	return true;
}

static bool
ascii_child_stop(ascii_child_t* child)
{
	int	status = 0;
	bool	ok = true;

	if (child->fd >= 0)
	{
		close(child->fd);
		child->fd = -1;
	}
	if (child->pid > 0)
	{
		for (u32 i = 0; i < 100; i++)
		{
			pid_t done = waitpid(child->pid, &status, WNOHANG);

			if (done == child->pid)
			{
				ok = WIFEXITED(status) ? WEXITSTATUS(status) == 0 : WIFSIGNALED(status);
				child->pid = -1;
				return ok;
			}
			usleep(10000);
		}
		kill(child->pid, SIGTERM);
		waitpid(child->pid, &status, 0);
		ok = WIFEXITED(status) ? WEXITSTATUS(status) == 0 : WIFSIGNALED(status);
		child->pid = -1;
	}
	return ok;
}

#if defined(__linux__)
static bool
ascii_child_cpu_ms(pid_t pid, u64* out_ms)
{
	char	path[64];
	FILE*	f;
	long	utime = 0;
	long	stime = 0;
	long	ticks;

	if (!out_ms || pid <= 0)
		return false;
	snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
	f = fopen(path, "r");
	if (!f)
		return false;
	{
		int	c;
		int	depth = 0;

		while ((c = fgetc(f)) != EOF)
		{
			if (c == '(') depth++;
			if (c == ')')
			{
				depth--;
				if (depth <= 0) break;
			}
		}
		if (c == EOF)
		{
			fclose(f);
			return false;
		}
	}
	if (fscanf(f,
		   " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %ld %ld",
		   &utime, &stime) != 2)
	{
		fclose(f);
		return false;
	}
	fclose(f);
	ticks = sysconf(_SC_CLK_TCK);
	if (ticks <= 0)
		return false;
	*out_ms = (u64)(utime + stime) * 1000ULL / (u64)ticks;
	return true;
}
#endif

TEST(AsciiServerPendingDelayedResponseHangupStopsWithoutSpin)
{
	/* Issue #51: POLLHUP with an undeliverable delayed response must stop the
	 * server, not spin through poll() until the delay expires. */
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = ascii_make_device(arena, 1, 0x3344);
	ascii_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		frame[MODBUS_ASCII_MAX_FRAME];
	u32		flen = 0;
#if defined(__linux__)
	u64		cpu_before = 0;
	u64		cpu_after = 0;
#endif
	int		status = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	dev->fault.kind     = FAULT_DELAY;
	dev->fault.delay_ms = 500;
	dev->fault.percent  = 100;
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(ascii_child_start(&child, devices, 1));
	}

	flen = modbus_ascii_encode(frame, 1, req, sizeof(req));
	ok = flen > 0 && send_all_fd(child.fd, frame, flen);
	usleep(120000); /* let the server queue the delayed response */
#if defined(__linux__)
	if (ok) ok = ascii_child_cpu_ms(child.pid, &cpu_before);
#endif
	if (ok)
	{
		close(child.fd);
		child.fd = -1;
	}
	/* The response is due 500 ms after the request; the server must be gone
	 * well before then and must not burn CPU on repeated POLLHUP. */
	if (ok) usleep(350000);
#if defined(__linux__)
	if (ok && !ascii_child_cpu_ms(child.pid, &cpu_after))
		cpu_after = cpu_before; /* process already exited; no spin */
	if (ok) ok = (cpu_after - cpu_before) < 200;
#endif
	if (ok)
	{
		pid_t done = waitpid(child.pid, &status, WNOHANG);

		ok = done == child.pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
		if (done == child.pid)
			child.pid = -1;
	}

	ASSERT_EQ(ascii_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(AsciiServerEofAfterDelayedRequestSendsResponse)
{
	/* EOF without HUP must still drain the queued delayed response. */
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = ascii_make_device(arena, 1, 0x5678);
	ascii_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		frame[MODBUS_ASCII_MAX_FRAME];
	u8		pdu[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		flen = 0;
	u32		pdu_len = 0;
	u64		start = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	dev->fault.kind     = FAULT_DELAY;
	dev->fault.delay_ms = 200;
	dev->fault.percent  = 100;
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(ascii_child_start(&child, devices, 1));
	}

	flen = modbus_ascii_encode(frame, 1, req, sizeof(req));
	start = clock_now_ms();
	ok = flen > 0 && send_all_fd(child.fd, frame, flen);
	if (ok) ok = shutdown(child.fd, SHUT_WR) == 0;
	if (ok) ok = recv_ascii_resp(child.fd, &addr, pdu, &pdu_len, 1500);
	if (ok) ok = (clock_now_ms() - start) >= 180;
	if (ok)
		ok = addr == 1 && pdu_len == 4 && pdu[0] == FC_READ_HOLDING_REGISTERS &&
		     get_be16(pdu + 2) == 0x5678;

	ASSERT_EQ(ascii_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}
