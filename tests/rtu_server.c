#define _GNU_SOURCE
#include "vtest.h"
#include "modbus/bytes.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "net/rtu_framer.h"
#include "net/rtu_server.h"
#include "device/device.h"
#include "util/clock.h"
#include "vlib/arena.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <stdio.h>
#include <unistd.h>

typedef struct rtu_child_t rtu_child_t;
struct rtu_child_t
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
recv_none_fd(int fd, u32 timeout_ms)
{
	struct pollfd	pfd = { .fd = fd, .events = POLLIN };
	int		rc  = poll(&pfd, 1, (int)timeout_ms);

	if (rc <= 0)
		return true;
	if (pfd.revents & POLLIN)
	{
		u8	buf[16];
		ssize_t	got = read(fd, buf, sizeof(buf));

		return got <= 0;
	}
	return true;
}

static bool
rtu_child_start_opts(rtu_child_t* child, device_t** devices, u32 device_count, u32 frame_gap_ms, u32 baud)
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
		rtu_server_options_t opts = {
			.fd           = fds[1],
			.seed         = 1,
			.frame_gap_ms = frame_gap_ms,
			.baud         = baud,
			.strict_t15   = false,
			.devices      = devices,
			.device_count = device_count,
		};

		close(fds[0]);
		(void)rtu_server_run(&opts);
		close(fds[1]);
		_exit(0);
	}

	close(fds[1]);
	child->fd = fds[0];
	return true;
}

static bool
rtu_child_start_opts_flags(rtu_child_t* child, device_t** devices, u32 device_count, u32 frame_gap_ms,
			   u32 baud, bool strict_t15, bool broadcast_execute)
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
		rtu_server_options_t opts = {
			.fd           = fds[1],
			.seed         = 1,
			.frame_gap_ms = frame_gap_ms,
			.baud         = baud,
			.strict_t15   = strict_t15,
			.broadcast_execute = broadcast_execute,
			.devices      = devices,
			.device_count = device_count,
		};

		close(fds[0]);
		(void)rtu_server_run(&opts);
		close(fds[1]);
		_exit(0);
	}

	close(fds[1]);
	child->fd = fds[0];
	return true;
}

static bool
rtu_child_start(rtu_child_t* child, device_t** devices, u32 device_count, u32 frame_gap_ms)
{
	return rtu_child_start_opts(child, devices, device_count, frame_gap_ms, 0);
}

static bool
rtu_child_stop(rtu_child_t* child)
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

static bool
send_rtu_req(int fd, u8 addr, const u8* pdu, u32 pdu_len)
{
	u8	frame[MODBUS_RTU_MAX_FRAME];
	u32	len = modbus_rtu_encode(frame, addr, pdu, pdu_len);

	if (len == 0)
		return false;
	return send_all_fd(fd, frame, len);
}

static bool
recv_rtu_resp_buf(int fd, u8* buf, u32* got, u8* addr, u8* pdu, u32* pdu_len, u32 timeout_ms)
{
	u64	deadline = clock_now_ms() + timeout_ms;

	while (*got < MODBUS_RTU_MAX_FRAME)
	{
		struct pollfd	pfd = { .fd = fd, .events = POLLIN };
		u64		now = clock_now_ms();
		int		wait_ms;
		ssize_t		n;
		const u8*	out_pdu = NULL;
		u32		out_len = 0;
		u8		out_addr = 0;

		for (u32 try_len = 4; try_len <= *got; try_len++)
		{
			if (modbus_rtu_decode(buf, try_len, &out_addr, &out_pdu, &out_len))
			{
				*addr = out_addr;
				*pdu_len = out_len;
				memcpy(pdu, out_pdu, out_len);
				memmove(buf, buf + try_len, *got - try_len);
				*got -= try_len;
				return true;
			}
		}

		if (now >= deadline)
			return false;
		wait_ms = (int)(deadline - now);
		if (poll(&pfd, 1, wait_ms) <= 0)
			continue;
		if (!(pfd.revents & (POLLIN | POLLHUP)))
		{
			if (pfd.revents & (POLLERR | POLLNVAL))
				return false;
			continue;
		}
		n = read(fd, buf + *got, MODBUS_RTU_MAX_FRAME - *got);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		*got += (u32)n;
	}
	return false;
}

static bool
recv_rtu_resp(int fd, u8* addr, u8* pdu, u32* pdu_len, u32 timeout_ms)
{
	u8	buf[MODBUS_RTU_MAX_FRAME];
	u32	got = 0;

	return recv_rtu_resp_buf(fd, buf, &got, addr, pdu, pdu_len, timeout_ms);
}

static device_t*
make_device(arena_t* arena, u8 unit, space_kind_t space, u32 address, u16 value)
{
	device_t*	dev = device_new(arena, 0);
	point_t		p   = { 0 };

	if (!dev)
		return NULL;
	dev->unit_id = unit;
	strcpy(dev->name, "rtu-test");
	p.space = space;
	p.address = address;
	p.type = VT_U16;
	p.kind = POINT_STATIC;
	p.static_value = (f64)value;
	if (device_add_point(dev, &p) != NULL)
		return NULL;
	return dev;
}

TEST(RtuGapElapsedHandlesStaleClockSample)
{
	/* The loop samples the clock before read_available(), so last_rx_ms can be
	 * newer than now_ms. That must not look like an elapsed gap. */
	ASSERT_EQ(rtu_gap_elapsed(1000, 1001, 20) == false);
	ASSERT_EQ(rtu_gap_elapsed(0, ~0ULL, 20) == false);
	ASSERT_EQ(rtu_gap_elapsed(1000, 1000, 20) == false);
	ASSERT_EQ(rtu_gap_elapsed(1019, 1000, 20) == false);
	ASSERT_EQ(rtu_gap_elapsed(1020, 1000, 20) == true);
}

TEST(RtuServerReadHoldingRegisters)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x1234);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 5));
	}

	ok = send_rtu_req(child.fd, 1, req, sizeof(req));
	ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 500);
	ok = ok && addr == 1 && len == 4 && resp[0] == FC_READ_HOLDING_REGISTERS &&
	     resp[1] == 2 && get_be16(resp + 2) == 0x1234;

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerWriteAndReadBack)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 1, 0);
	rtu_child_t	child = { 0 };
	u8		write_req[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x01, 0x00, 0x2A };
	u8		read_req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x01, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 5));
	}

	ok = send_rtu_req(child.fd, 1, write_req, sizeof(write_req));
	ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 500);
	ok = ok && addr == 1 && len == 5 && resp[0] == FC_WRITE_SINGLE_REGISTER;
	if (ok)
	{
		ok = send_rtu_req(child.fd, 1, read_req, sizeof(read_req));
		ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 500);
		ok = ok && addr == 1 && len == 4 && get_be16(resp + 2) == 0x002A;
	}

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerBadCrcIsIgnored)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 7);
	rtu_child_t	child = { 0 };
	u8		frame[MODBUS_RTU_MAX_FRAME];
	u8		pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u32		frame_len;
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 5));
	}

	frame_len = modbus_rtu_encode(frame, 1, pdu, sizeof(pdu));
	frame[frame_len - 1] ^= 0xFF;
	ok = send_all_fd(child.fd, frame, frame_len);
	if (ok) ok = recv_none_fd(child.fd, 150);
	if (ok) ok = send_rtu_req(child.fd, 1, pdu, sizeof(pdu));
	if (ok) ok = recv_rtu_resp(child.fd, &addr, resp, &len, 500);
	if (ok) ok = addr == 1 && get_be16(resp + 2) == 7;

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerUnknownUnitNoResponse)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 9);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 5));
	}

	ok = send_rtu_req(child.fd, 2, req, sizeof(req));
	ok = ok && recv_none_fd(child.fd, 150);
	if (ok)
	{
		ok = send_rtu_req(child.fd, 1, req, sizeof(req));
		ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 500);
		ok = ok && addr == 1;
	}

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerBroadcastNoResponse)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0x00, 0x05 };
	u8		read_req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 5));
	}

	ok = send_rtu_req(child.fd, 0, req, sizeof(req));
	ok = ok && recv_none_fd(child.fd, 150);
	if (ok)
	{
		ok = send_rtu_req(child.fd, 1, read_req, sizeof(read_req));
		ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 500);
		ok = ok && addr == 1 && get_be16(resp + 2) == 0;
	}

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerBroadcastExecuteWritesAllDevicesWithoutResponse)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev1  = make_device(arena, 1, SPACE_HOLDING, 0, 0);
	device_t*	dev2  = make_device(arena, 2, SPACE_HOLDING, 0, 0);
	rtu_child_t	child = { 0 };
	u8		write_req[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0x00, 0x2A };
	u8		read_req[]  = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len  = 0;
	bool		ok   = false;

	ASSERT_EQ(dev1 != NULL && dev2 != NULL);
	{
		device_t* devices[] = { dev1, dev2 };
		ASSERT_EQ(rtu_child_start_opts_flags(&child, devices, 2, 5, 0, false, true));
	}

	ok = send_rtu_req(child.fd, 0, write_req, sizeof(write_req));
	ok = ok && recv_none_fd(child.fd, 200);
	for (u8 unit = 1; unit <= 2 && ok; unit++)
	{
		ok = send_rtu_req(child.fd, unit, read_req, sizeof(read_req));
		ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 500);
		ok = ok && addr == unit && get_be16(resp + 2) == 0x002A;
	}
	ok = ok && recv_none_fd(child.fd, 80);

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerExceptionResponseHasRtuCrc)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x05, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 5));
	}

	ok = send_rtu_req(child.fd, 1, req, sizeof(req));
	ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 500);
	ok = ok && addr == 1 && len == 2 && resp[0] == (u8)(FC_READ_HOLDING_REGISTERS | 0x80) &&
	     resp[1] == EXC_ILLEGAL_DATA_ADDRESS;

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

#if defined(__linux__)
static bool
rtu_child_cpu_ms(pid_t pid, u64* out_ms)
{
	char		path[64];
	FILE*		f;
	long		utime = 0;
	long		stime = 0;
	long		ticks;

	if (!out_ms || pid <= 0)
		return false;
	snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
	f = fopen(path, "r");
	if (!f)
		return false;
	/* comm may contain spaces; scan after last ')' */
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

TEST(RtuServerShortJunkThenValidRequest)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0xBEEF);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	bool		ok = true;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));
	}

	for (u32 junk_len = 1; junk_len <= 3 && ok; junk_len++)
	{
		u8	junk[3] = { 0xAA, 0xBB, 0xCC };
#if defined(__linux__)
		u64	cpu_before = 0;
		u64	cpu_after = 0;
#endif

		ok = send_all_fd(child.fd, junk, junk_len);
		ok = ok && recv_none_fd(child.fd, 60);
		/* After gap elapses, server must drop short junk and return to poll(). */
#if defined(__linux__)
		if (ok) ok = rtu_child_cpu_ms(child.pid, &cpu_before);
		if (ok) usleep(200000);
		if (ok) ok = rtu_child_cpu_ms(child.pid, &cpu_after);
		if (ok) ok = (cpu_after - cpu_before) < 150;
#else
		if (ok) usleep(200000);
#endif

		if (ok) ok = send_rtu_req(child.fd, 1, req, sizeof(req));
		if (ok) ok = recv_rtu_resp(child.fd, &addr, resp, &len, 500);
		if (ok)
			ok = addr == 1 && len == 4 &&
			     resp[0] == FC_READ_HOLDING_REGISTERS &&
			     resp[1] == 2 && get_be16(resp + 2) == 0xBEEF;
	}

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerValidFramePlusTrailingByteThenValidRequest)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x1122);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		frame[MODBUS_RTU_MAX_FRAME + 1];
	u32		frame_len;
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));
	}

	frame_len = modbus_rtu_encode(frame, 1, req, sizeof(req));
	ASSERT(frame_len > 0 && frame_len < sizeof(frame));
	frame[frame_len] = 0x00; /* stray trailing byte in same write */
	ok = send_all_fd(child.fd, frame, frame_len + 1);
	ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 500);
	ok = ok && addr == 1 && len == 4 && get_be16(resp + 2) == 0x1122;

	/* Stray byte must be discarded after gap; next request gets one response. */
	if (ok) ok = send_rtu_req(child.fd, 1, req, sizeof(req));
	if (ok) ok = recv_rtu_resp(child.fd, &addr, resp, &len, 500);
	if (ok) ok = addr == 1 && len == 4 && get_be16(resp + 2) == 0x1122;
	if (ok) ok = recv_none_fd(child.fd, 80);

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerCoalescedValidRequests)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x3344);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		frame[MODBUS_RTU_MAX_FRAME * 2];
	u8		buf[MODBUS_RTU_MAX_FRAME];
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	u32		flen = 0;
	u32		got = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	{
		point_t p = { 0 };
		device_t* devices[] = { dev };

		p.space = SPACE_HOLDING;
		p.address = 1;
		p.type = VT_U16;
		p.kind = POINT_STATIC;
		p.static_value = 0x5566;
		ASSERT_EQ(device_add_point(dev, &p) == NULL);
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));
	}

	flen = modbus_rtu_encode(frame, 1, req, sizeof(req));
	req[2] = 0x01; /* read address 1 */
	flen += modbus_rtu_encode(frame + flen, 1, req, sizeof(req));
	/* Two full frames in one write; server processes first, then next after gap. */
	ok = send_all_fd(child.fd, frame, flen);
	ASSERT_EQ(ok);
	ASSERT_EQ(recv_rtu_resp_buf(child.fd, buf, &got, &addr, resp, &len, 800));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(get_be16(resp + 2) == 0x3344);
	ASSERT_EQ(recv_rtu_resp_buf(child.fd, buf, &got, &addr, resp, &len, 800));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(get_be16(resp + 2) == 0x5566);
	ASSERT_EQ(recv_none_fd(child.fd, 100));

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerValidRequestFollowedByPartialFrame)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x7788);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		wire[MODBUS_RTU_MAX_FRAME * 2];
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	u32		first_len = 0;
	u32		second_len = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));
	}

	first_len = modbus_rtu_encode(wire, 1, req, sizeof(req));
	second_len = modbus_rtu_encode(wire + first_len, 1, req, sizeof(req));
	ASSERT(second_len > 3);
	/* First valid frame plus partial second frame in one write. */
	ok = send_all_fd(child.fd, wire, first_len + 3);
	ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 800);
	ok = ok && addr == 1 && get_be16(resp + 2) == 0x7788;
	/* Partial tail must be discarded after gap; server recovers. */
	if (ok) ok = recv_none_fd(child.fd, 120);
	if (ok) ok = send_rtu_req(child.fd, 1, req, sizeof(req));
	if (ok) ok = recv_rtu_resp(child.fd, &addr, resp, &len, 800);
	if (ok) ok = addr == 1 && get_be16(resp + 2) == 0x7788;
	if (ok) ok = recv_none_fd(child.fd, 80);

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerPendingDelayedResponsePeerCloseNoSpin)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x3344);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
#if defined(__linux__)
	u64		cpu_before = 0;
	u64		cpu_after = 0;
#endif
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	dev->fault.kind     = FAULT_DELAY;
	dev->fault.delay_ms = 500;
	dev->fault.percent  = 100;
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 5));
	}

	ok = send_rtu_req(child.fd, 1, req, sizeof(req));
	/* Let the server observe frame gap and queue the delayed response. */
	if (ok) usleep(120000);
#if defined(__linux__)
	if (ok) ok = rtu_child_cpu_ms(child.pid, &cpu_before);
#endif
	if (ok)
	{
		close(child.fd);
		child.fd = -1;
	}
	/* Before issue #19 fix, poll() on a hung-up fd with pending response
	 * returned immediately and burned CPU until the delay elapsed. */
	if (ok) usleep(350000);
#if defined(__linux__)
	if (ok && !rtu_child_cpu_ms(child.pid, &cpu_after))
		cpu_after = cpu_before; /* process already exited; no spin */
	/* Tolerant for ASan/CI scheduling noise. */
	if (ok) ok = (cpu_after - cpu_before) < 200;
#endif

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerPendingDelayedResponseHangupWithRequestNoSpin)
{
	/* Issue #40: request and POLLHUP seen in the same poll; the queued delayed
	 * response cannot be delivered and must not cause a busy loop. */
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x3344);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		frame[MODBUS_RTU_MAX_FRAME];
	u32		flen;
#if defined(__linux__)
	u64		cpu_before = 0;
	u64		cpu_after = 0;
#endif
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	dev->fault.kind     = FAULT_DELAY;
	dev->fault.delay_ms = 500;
	dev->fault.percent  = 100;
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));
	}

	flen = modbus_rtu_encode(frame, 1, req, sizeof(req));
	ok = send_all_fd(child.fd, frame, flen);
	if (ok)
	{
		close(child.fd);
		child.fd = -1;
	}
	if (ok) usleep(120000);
#if defined(__linux__)
	if (ok) ok = rtu_child_cpu_ms(child.pid, &cpu_before);
#endif
	if (ok) usleep(350000);
#if defined(__linux__)
	if (ok && !rtu_child_cpu_ms(child.pid, &cpu_after))
		cpu_after = cpu_before;
	if (ok) ok = (cpu_after - cpu_before) < 200;
#endif
	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerPrefixValidCrcDoesNotSplitLongFc16)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = device_new(arena, 0);
	rtu_child_t	child = { 0 };
	u8		pdu[6 + 123 * 2];
	u8		frame[MODBUS_RTU_MAX_FRAME];
	u8		req[MODBUS_RTU_MAX_FRAME];
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	u32		frame_len = 0;
	u32		prefix_len = 0;
	bool		found = false;

	ASSERT_EQ(dev != NULL);
	dev->unit_id = 1;
	strcpy(dev->name, "rtu-fc16");

	for (u32 i = 0; i < 123; i++)
	{
		point_t p = { 0 };

		p.space = SPACE_HOLDING;
		p.address = i;
		p.type = VT_U16;
		p.kind = POINT_STATIC;
		p.static_value = (f64)(0x2000 + i);
		ASSERT_EQ(device_add_point(dev, &p) == NULL);
	}

	pdu[0] = FC_WRITE_MULTIPLE_REGISTERS;
	put_be16(pdu + 1, 0);   /* start address */
	put_be16(pdu + 3, 123); /* quantity */
	pdu[5] = 123 * 2;
	for (u32 i = 0; i < 123; i++)
		put_be16(pdu + 6 + i * 2, (u16)(0x3000 + i));

	frame_len = modbus_rtu_encode(frame, 1, pdu, sizeof(pdu));
	ASSERT_EQ(frame_len == 1 + sizeof(pdu) + 2);

	/* Construct a request whose full frame is valid, but some shorter prefix also
	 * has a valid CRC. The bug accepted the shortest prefix. Brute-force a data
	 * byte so the full FC16 request remains valid (do not touch FC/addr/qty/byte
	 * count/CRC fields). */
	memcpy(req, frame, frame_len);
	for (u32 data_idx = 8; data_idx < frame_len - 2 && !found; data_idx++)
	{
		u8 saved = req[data_idx];

		for (u32 v = 0; v <= 0xFF; v++)
		{
			u16 crc;

			req[data_idx] = (u8)v;
			crc = modbus_crc16(req, frame_len - 2);
			req[frame_len - 2] = (u8)(crc & 0xFF);
			req[frame_len - 1] = (u8)(crc >> 8);

			for (u32 prefix = 4; prefix < frame_len; prefix++)
			{
				u8		paddr = 0;
				const u8*	ppdu = NULL;
				u32		plen = 0;

				if (modbus_rtu_decode(req, prefix, &paddr, &ppdu, &plen))
				{
					prefix_len = prefix;
					found = true;
					break;
				}
			}
			if (found)
				break;
		}
		if (!found)
			req[data_idx] = saved;
	}
	if (!found)
	{
		u16 crc = modbus_crc16(frame, frame_len - 2);

		frame[frame_len - 2] = (u8)(crc & 0xFF);
		frame[frame_len - 1] = (u8)(crc >> 8);
		memcpy(req, frame, frame_len);
	}
	ASSERT_EQ(found);

	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));
	}

	(void)prefix_len;
	ASSERT_EQ(send_all_fd(child.fd, req, frame_len));
	ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 800));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(len == 5);
	ASSERT_EQ(resp[0] == FC_WRITE_MULTIPLE_REGISTERS);
	ASSERT_EQ(get_be16(resp + 1) == 0);
	ASSERT_EQ(get_be16(resp + 3) == 123);

	{
		u8 read_req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 123 };

		ASSERT_EQ(send_rtu_req(child.fd, 1, read_req, sizeof(read_req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 800));
		ASSERT_EQ(addr == 1);
		ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
		ASSERT_EQ(resp[1] == 123 * 2);
		for (u32 i = 0; i < 123; i++)
		{
			u16 expected = get_be16(req + 7 + i * 2); /* frame: addr+fc+start+qty+bc + data */
			u16 got = get_be16(resp + 2 + i * 2);

			ASSERT_EQ(got == expected);
		}
	}

	/* Sanity: the prefix itself would be a malformed/short request if processed. */
	ASSERT(prefix_len < frame_len);

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
}

TEST(RtuServerFragmentedByteByByteWithinGap)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x99AA);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		frame[MODBUS_RTU_MAX_FRAME];
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	u32		flen = 0;
	bool		ok = true;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 80));
	}

	flen = modbus_rtu_encode(frame, 1, req, sizeof(req));
	for (u32 i = 0; i < flen && ok; i++)
	{
		ok = send_all_fd(child.fd, frame + i, 1);
		if (ok && i + 1 < flen)
			usleep(5000); /* well below 80ms gap */
	}
	if (ok) ok = recv_rtu_resp(child.fd, &addr, resp, &len, 1000);
	if (ok)
		ok = addr == 1 && len == 4 && resp[0] == FC_READ_HOLDING_REGISTERS &&
		     get_be16(resp + 2) == 0x99AA;

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerFragmentedAcrossGapDoesNotFormValidFrame)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x1234);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		frame[MODBUS_RTU_MAX_FRAME];
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	u32		flen = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 30));
	}

	flen = modbus_rtu_encode(frame, 1, req, sizeof(req));
	ASSERT(flen > 4);
	ok = send_all_fd(child.fd, frame, flen / 2);
	if (ok) usleep(120000); /* longer than gap: first half discarded */
	if (ok) ok = send_all_fd(child.fd, frame + flen / 2, flen - flen / 2);
	/* Second half alone is not a valid frame; no response. */
	if (ok) ok = recv_none_fd(child.fd, 150);
	/* Server recovers on a clean frame. */
	if (ok) ok = send_rtu_req(child.fd, 1, req, sizeof(req));
	if (ok) ok = recv_rtu_resp(child.fd, &addr, resp, &len, 800);
	if (ok) ok = addr == 1 && get_be16(resp + 2) == 0x1234;

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerBaudDerivedGapFragments)
{
	/* 9600 baud -> t3.5 ~4.01ms. Byte-stream read() boundaries cannot model true
	 * UART char gaps; t1.5 discard is opt-in and off by default, so fragmented
	 * delivery within t3.5 should be accepted while still exercising baud-derived
	 * inter-frame timing. */
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x55AA);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		frame[MODBUS_RTU_MAX_FRAME];
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	u32		flen = 0;
	bool		ok = true;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start_opts(&child, devices, 1, 0, 9600));
	}

	flen = modbus_rtu_encode(frame, 1, req, sizeof(req));
	for (u32 i = 0; i < flen && ok; i += 2)
	{
		u32 n = (i + 2 <= flen) ? 2 : (flen - i);

		ok = send_all_fd(child.fd, frame + i, n);
		if (ok && i + n < flen)
			usleep(1000); /* well within t3.5 */
	}
	if (ok) ok = recv_rtu_resp(child.fd, &addr, resp, &len, 1000);
	if (ok) ok = addr == 1 && get_be16(resp + 2) == 0x55AA;

	/* Split across a gap larger than t3.5 should not produce a response. */
	if (ok) ok = send_all_fd(child.fd, frame, flen / 2);
	if (ok) usleep(20000);
	if (ok) ok = send_all_fd(child.fd, frame + flen / 2, flen - flen / 2);
	if (ok) ok = recv_none_fd(child.fd, 120);
	if (ok) ok = send_rtu_req(child.fd, 1, req, sizeof(req));
	if (ok) ok = recv_rtu_resp(child.fd, &addr, resp, &len, 800);
	if (ok) ok = addr == 1 && get_be16(resp + 2) == 0x55AA;

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerBaudSplitFrameThreeMsIsAnswered)
{
	/* Acceptance: a frame sent in two writes 3 ms apart at --baud 115200 over a
	 * byte stream/PTY-like fd must be answered. t1.5 discard is opt-in and must
	 * not break valid frames at read() boundaries. */
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x66BB);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		frame[MODBUS_RTU_MAX_FRAME];
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	u32		flen = 0;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start_opts(&child, devices, 1, 0, 115200));
	}

	flen = modbus_rtu_encode(frame, 1, req, sizeof(req));
	ASSERT(flen > 4);
	ASSERT_EQ(send_all_fd(child.fd, frame, flen / 2));
	usleep(3000);
	ASSERT_EQ(send_all_fd(child.fd, frame + flen / 2, flen - flen / 2));
	ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 2000));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(get_be16(resp + 2) == 0x66BB);

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
}

TEST(RtuServerBaudT15GapDiscardsPartialFrame)
{
	/* 1200 baud -> t1.5 ~13.75ms, t3.5 ~32.08ms. With strict-t15 opt-in, a gap
	 * between t1.5 and t3.5 inside an incomplete frame discards it, but the server
	 * must recover on the next clean frame. Default behavior no longer enforces t1.5. */
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x7788);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		frame[MODBUS_RTU_MAX_FRAME];
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	u32		flen = 0;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start_opts_flags(&child, devices, 1, 0, 1200, true, false));
	}

	flen = modbus_rtu_encode(frame, 1, req, sizeof(req));
	ASSERT(flen > 4);
	ASSERT_EQ(send_all_fd(child.fd, frame, 2));
	usleep(5000); /* let server read first chunk */
	usleep(20000); /* total > t1.5 (13.75ms), < t3.5 (32.08ms) */
	ASSERT_EQ(send_all_fd(child.fd, frame + 2, flen - 2));
	ASSERT_EQ(recv_none_fd(child.fd, 120));

	ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
	ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(get_be16(resp + 2) == 0x7788);

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
}

TEST(RtuServerFrameGapOverrideBeatsBaud)
{
	/* Baud-derived t3.5 would be ~4ms at 9600, but explicit frame-gap-ms override
	 * should keep fragments together for the configured gap. */
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x3344);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		frame[MODBUS_RTU_MAX_FRAME];
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	u32		flen = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start_opts(&child, devices, 1, 80, 9600));
	}

	flen = modbus_rtu_encode(frame, 1, req, sizeof(req));
	ASSERT(flen > 4);
	ok = send_all_fd(child.fd, frame, flen / 2);
	if (ok)
	{
		u8 tmp_a = 0, tmp_p[MODBUS_MAX_PDU];
		u32 tmp_l = 0;
		ok = !recv_rtu_resp(child.fd, &tmp_a, tmp_p, &tmp_l, 30); /* > t3.5, < override */
	}
	if (ok) usleep(30000);
	if (ok) ok = send_all_fd(child.fd, frame + flen / 2, flen - flen / 2);
	if (ok) ok = recv_rtu_resp(child.fd, &addr, resp, &len, 1200);
	if (ok) ok = addr == 1 && get_be16(resp + 2) == 0x3344;

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerMaxSizeFc16AndFc03)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = device_new(arena, 0);
	rtu_child_t	child = { 0 };
	u8		pdu[6 + 123 * 2];
	u8		read_req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 125 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	dev->unit_id = 1;
	strcpy(dev->name, "rtu-max");
	for (u32 i = 0; i < 125; i++)
	{
		point_t p = { 0 };

		p.space = SPACE_HOLDING;
		p.address = i;
		p.type = VT_U16;
		p.kind = POINT_STATIC;
		p.static_value = (f64)i;
		ASSERT_EQ(device_add_point(dev, &p) == NULL);
	}

	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));
	}

	pdu[0] = FC_WRITE_MULTIPLE_REGISTERS;
	put_be16(pdu + 1, 0);
	put_be16(pdu + 3, 123);
	pdu[5] = 123 * 2;
	for (u32 i = 0; i < 123; i++)
		put_be16(pdu + 6 + i * 2, (u16)(0xA000 + i));

	ok = send_rtu_req(child.fd, 1, pdu, sizeof(pdu));
	ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 1000);
	ok = ok && addr == 1 && len == 5 && resp[0] == FC_WRITE_MULTIPLE_REGISTERS;
	ok = ok && get_be16(resp + 1) == 0 && get_be16(resp + 3) == 123;

	if (ok)
	{
		ok = send_rtu_req(child.fd, 1, read_req, sizeof(read_req));
		ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 1000);
		ok = ok && addr == 1 && resp[0] == FC_READ_HOLDING_REGISTERS && resp[1] == 125 * 2;
		for (u32 i = 0; i < 123 && ok; i++)
			ok = get_be16(resp + 2 + i * 2) == (u16)(0xA000 + i);
		for (u32 i = 123; i < 125 && ok; i++)
			ok = get_be16(resp + 2 + i * 2) == (u16)i;
	}

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerDelayAndDropFaultsOverRtu)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev_delay = make_device(arena, 1, SPACE_HOLDING, 0, 0x1111);
	device_t*	dev_drop  = make_device(arena, 2, SPACE_HOLDING, 0, 0x2222);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	u64		start = 0;
	bool		ok = false;

	ASSERT_EQ(dev_delay != NULL && dev_drop != NULL);
	dev_delay->fault.kind = FAULT_DELAY;
	dev_delay->fault.delay_ms = 250;
	dev_delay->fault.percent = 100;
	dev_drop->fault.kind = FAULT_DROP;
	dev_drop->fault.percent = 100;

	{
		device_t* devices[] = { dev_delay, dev_drop };
		ASSERT_EQ(rtu_child_start(&child, devices, 2, 20));
	}

	/* Drop: no response, but server stays healthy for other units. */
	ok = send_rtu_req(child.fd, 2, req, sizeof(req));
	ok = ok && recv_none_fd(child.fd, 200);

	/* Delay: response arrives after configured delay. */
	if (ok)
	{
		start = clock_now_ms();
		ok = send_rtu_req(child.fd, 1, req, sizeof(req));
		ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 1200);
		ok = ok && (clock_now_ms() - start) >= 200;
		ok = ok && addr == 1 && get_be16(resp + 2) == 0x1111;
	}

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

static bool
rtu_expect_fc03(rtu_child_t* child, u8 unit, u16 reg, u16 value)
{
	u8	req[] = { FC_READ_HOLDING_REGISTERS, 0, 0, 0, 1 };
	u8	resp[MODBUS_MAX_PDU];
	u8	addr = 0;
	u32	len = 0;

	put_be16(req + 1, reg);
	return send_rtu_req(child->fd, unit, req, sizeof(req)) &&
	       recv_rtu_resp(child->fd, &addr, resp, &len, 1200) &&
	       addr == unit && len == 4 && resp[0] == FC_READ_HOLDING_REGISTERS &&
	       resp[1] == 2 && get_be16(resp + 2) == value;
}

TEST(RtuServerSupportedFunctionCodesFraming)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev = device_new(arena, 0);
	rtu_child_t	child = { 0 };

	ASSERT_EQ(dev != NULL);
	dev->unit_id = 1;
	strcpy(dev->name, "rtu-fc-all");
	dev->server_id = 0x2A;
	dev->run_indicator = 0xFF;

	{
		static const struct { space_kind_t s; u32 a; value_type_t t; u16 v; } seeds[] = {
			{ SPACE_COIL, 0, VT_BOOL, 1 }, { SPACE_COIL, 1, VT_BOOL, 0 },
			{ SPACE_DISCRETE, 0, VT_BOOL, 1 },
			{ SPACE_INPUT, 0, VT_U16, 0x1122 }, { SPACE_INPUT, 1, VT_U16, 0x3344 },
			{ SPACE_HOLDING, 0, VT_U16, 0x0001 }, { SPACE_HOLDING, 1, VT_U16, 0x0002 },
			{ SPACE_HOLDING, 2, VT_U16, 0x0003 }, { SPACE_HOLDING, 3, VT_U16, 0x0004 },
			{ SPACE_HOLDING, 8, VT_U16, 0x00FF },
		};
		for (u32 i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
		{
			point_t p = { 0 };
			p.space = seeds[i].s; p.address = seeds[i].a; p.type = seeds[i].t;
			p.kind = POINT_STATIC; p.static_value = seeds[i].v;
			ASSERT_EQ(device_add_point(dev, &p) == NULL);
		}
		{
			u16 rec = 0xBEEF;
			ASSERT_EQ(device_add_file_record(dev, 1, 0, 1, &rec) == NULL);
		}
	}
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));
	}

	{
		u8 req[] = { FC_READ_COILS, 0x00, 0x00, 0x00, 0x02 };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len == 3);
		ASSERT_EQ(resp[0] == FC_READ_COILS && resp[1] == 1 && resp[2] == 0x01);
	}
	{
		u8 req[] = { FC_READ_DISCRETE_INPUTS, 0x00, 0x00, 0x00, 0x01 };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len == 3);
		ASSERT_EQ(resp[0] == FC_READ_DISCRETE_INPUTS && resp[1] == 1 && resp[2] == 0x01);
	}
	ASSERT_EQ(rtu_expect_fc03(&child, 1, 0, 0x0001));
	{
		u8 req[] = { FC_READ_INPUT_REGISTERS, 0x00, 0x00, 0x00, 0x02 };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len == 6);
		ASSERT_EQ(resp[0] == FC_READ_INPUT_REGISTERS && resp[1] == 4);
		ASSERT_EQ(get_be16(resp + 2) == 0x1122 && get_be16(resp + 4) == 0x3344);
	}
	{
		u8 req[] = { FC_WRITE_SINGLE_COIL, 0x00, 0x01, 0xFF, 0x00 };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len == 5);
		ASSERT_EQ(memcmp(resp, req, sizeof(req)) == 0);
	}
	{
		u8 req[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x02, 0x00, 0x2A };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len == 5);
		ASSERT_EQ(memcmp(resp, req, sizeof(req)) == 0);
		ASSERT_EQ(rtu_expect_fc03(&child, 1, 2, 0x002A));
	}
	{
		u8 req[] = { FC_READ_EXCEPTION_STATUS };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len == 2);
		ASSERT_EQ(resp[0] == FC_READ_EXCEPTION_STATUS && resp[1] == 0);
	}
	{
		u8 req[] = { FC_DIAGNOSTICS, 0x00, 0x00, 0x12, 0x34 };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len == sizeof(req));
		ASSERT_EQ(memcmp(resp, req, sizeof(req)) == 0);
	}
	{
		u8 req[] = { FC_GET_COM_EVENT_COUNTER };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len == 5);
		ASSERT_EQ(resp[0] == FC_GET_COM_EVENT_COUNTER);
		ASSERT_EQ(get_be16(resp + 1) == 0x0000);
	}
	{
		u8 req[] = { FC_GET_COM_EVENT_LOG };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len == 8);
		ASSERT_EQ(resp[0] == FC_GET_COM_EVENT_LOG && resp[1] == 6);
		ASSERT_EQ(get_be16(resp + 2) == 0x0000);
	}
	{
		u8 req[] = { FC_WRITE_MULTIPLE_COILS, 0x00, 0x00, 0x00, 0x02, 0x01, 0x02 };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len == 5);
		ASSERT_EQ(memcmp(resp, req, 5) == 0);
	}
	{
		u8 req[] = { FC_WRITE_MULTIPLE_REGISTERS, 0x00, 0x03, 0x00, 0x01, 0x02, 0x00, 0x99 };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len == 5);
		ASSERT_EQ(memcmp(resp, req, 5) == 0);
		ASSERT_EQ(rtu_expect_fc03(&child, 1, 3, 0x0099));
	}
	{
		u8 req[] = { FC_REPORT_SERVER_ID };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len == 4);
		ASSERT_EQ(resp[0] == FC_REPORT_SERVER_ID && resp[1] == 2);
		ASSERT_EQ(resp[2] == 0x2A && resp[3] == 0xFF);
	}
	{
		u8 req[] = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01 };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len == 6);
		ASSERT_EQ(resp[0] == FC_READ_FILE_RECORD && resp[1] == 4);
		ASSERT_EQ(resp[2] == 3 && resp[3] == 0x06);
		ASSERT_EQ(get_be16(resp + 4) == 0xBEEF);
	}
	{
		u8 req[] = { FC_WRITE_FILE_RECORD, 0x09, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x12, 0x34 };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len == sizeof(req));
		ASSERT_EQ(memcmp(resp, req, sizeof(req)) == 0);
	}
	{
		u8 req[] = { FC_MASK_WRITE_REGISTER, 0x00, 0x08, 0x00, 0x0F, 0x00, 0xF0 };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len == 7);
		ASSERT_EQ(memcmp(resp, req, sizeof(req)) == 0);
		ASSERT_EQ(rtu_expect_fc03(&child, 1, 8, 0x00FF));
	}
	{
		u8 req[] = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0x00, 0x77 };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len == 4);
		ASSERT_EQ(resp[0] == FC_READ_WRITE_MULTIPLE_REGISTERS && resp[1] == 2);
		ASSERT_EQ(get_be16(resp + 2) == 0x0001);
		ASSERT_EQ(rtu_expect_fc03(&child, 1, 1, 0x0077));
	}
	{
		u8 req[] = { FC_READ_FIFO_QUEUE, 0x00, 0x00 };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len == 2);
		ASSERT_EQ(resp[0] == (u8)(FC_READ_FIFO_QUEUE | 0x80));
		ASSERT_EQ(resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	}
	{
		u8 req[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x01, 0x00 };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len > 3);
		ASSERT_EQ(resp[0] == FC_ENCAPSULATED_INTERFACE_TRANSPORT && resp[1] == 0x0E);
		ASSERT_EQ(resp[2] == 0x01 && resp[3] == 0x81);
		ASSERT_EQ(resp[4] == 0x00 && resp[5] == 0x00 && resp[6] == 0x03);
		ASSERT_EQ(resp[7] == 0x00 && resp[8] == 10);
		ASSERT_EQ(memcmp(resp + 9, "modbus-sim", 10) == 0);
		ASSERT_EQ(resp[19] == 0x01 && resp[20] == 10);
		ASSERT_EQ(memcmp(resp + 21, "modbus-sim", 10) == 0);
		ASSERT_EQ(resp[31] == 0x02 && resp[32] == 3);
		ASSERT_EQ(memcmp(resp + 33, "1.0", 3) == 0);
	}

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
}

TEST(RtuServerMei13AndExtendedDeviceIdObjects)
{
	arena_t*	arena  = ARENA_ALLOC();
	device_t*	dev    = device_new(arena, 0);
	device_t*	devices[] = { dev };
	rtu_child_t	child  = { 0 };
	u8		pdu1[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D, 0x01, 0x00 };
	u8		pdu2[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x04, 0xA0 };
	u8		pdu3[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D, 0x03, 0x00 };
	u8		pdu4[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x04, 0x81 };
	u8		frame1[MODBUS_RTU_MAX_FRAME];
	u8		frame2[MODBUS_RTU_MAX_FRAME];
	u8		both[MODBUS_RTU_MAX_FRAME * 2];
	u8		buf[MODBUS_RTU_MAX_FRAME * 2] = { 0 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		f1 = 0;
	u32		f2 = 0;
	u32		got = 0;
	u32		len = 0;

	ASSERT_EQ(dev != NULL);
	dev->unit_id = 1;
	strcpy(dev->name, "rtu-fc43-extra");
	strcpy(dev->device_id[DEVICE_ID_VENDOR_NAME], "Acme");
	dev->extended_id[0].id = 0xA0;
	strcpy(dev->extended_id[0].value, "Second");
	dev->extended_id[1].id = 0x80;
	strcpy(dev->extended_id[1].value, "First");
	dev->extended_id_count = 2;
	ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));

	/* Both frames in one write: the framer must know MEI 0x0D is a 4-byte PDU. */
	f1 = modbus_rtu_encode(frame1, 1, pdu1, sizeof(pdu1));
	f2 = modbus_rtu_encode(frame2, 1, pdu2, sizeof(pdu2));
	ASSERT_EQ(f1 > 0 && f2 > 0);
	memcpy(both, frame1, f1);
	memcpy(both + f1, frame2, f2);
	ASSERT_EQ(send_all_fd(child.fd, both, f1 + f2));

	ASSERT_EQ(recv_rtu_resp_buf(child.fd, buf, &got, &addr, resp, &len, 1200));
	ASSERT_EQ(addr == 1 && len == 30);
	ASSERT_EQ(resp[0] == FC_ENCAPSULATED_INTERFACE_TRANSPORT && resp[1] == 0x0D);
	ASSERT_EQ(resp[2] == 0x01 && resp[3] == 0x83 && resp[4] == 0x00 && resp[6] == 0x03);
	ASSERT_EQ(resp[7] == 0x00 && resp[8] == 4 && memcmp(resp + 9, "Acme", 4) == 0);
	ASSERT_EQ(resp[13] == 0x01 && resp[14] == 10);
	ASSERT_EQ(resp[25] == 0x02 && resp[26] == 3 && memcmp(resp + 27, "1.0", 3) == 0);

	ASSERT_EQ(recv_rtu_resp_buf(child.fd, buf, &got, &addr, resp, &len, 1200));
	ASSERT_EQ(addr == 1 && len == 15);
	ASSERT_EQ(resp[0] == FC_ENCAPSULATED_INTERFACE_TRANSPORT && resp[1] == 0x0E);
	ASSERT_EQ(resp[2] == 0x04 && resp[3] == 0x83 && resp[6] == 0x01);
	ASSERT_EQ(resp[7] == 0xA0 && resp[8] == 6 && memcmp(resp + 9, "Second", 6) == 0);

	ASSERT_EQ(send_rtu_req(child.fd, 1, pdu3, sizeof(pdu3)));
	ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
	ASSERT_EQ(addr == 1 && len == 45);
	ASSERT_EQ(resp[1] == 0x0D && resp[3] == 0x83 && resp[6] == 0x05);
	ASSERT_EQ(resp[30] == 0x80 && resp[37] == 0xA0);

	ASSERT_EQ(send_rtu_req(child.fd, 1, pdu4, sizeof(pdu4)));
	ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
	ASSERT_EQ(addr == 1 && len == 2);
	ASSERT_EQ(resp[0] == (u8)(FC_ENCAPSULATED_INTERFACE_TRANSPORT | 0x80));
	ASSERT_EQ(resp[1] == EXC_ILLEGAL_DATA_ADDRESS);

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
}

TEST(RtuServerUnitIdEdges)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev1   = make_device(arena, 1, SPACE_HOLDING, 0, 0x0001);
	device_t*	dev247 = make_device(arena, 247, SPACE_HOLDING, 0, 0x00F7);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	bool		ok = false;

	ASSERT_EQ(dev1 != NULL && dev247 != NULL);
	{
		device_t* devices[] = { dev1, dev247 };
		ASSERT_EQ(rtu_child_start(&child, devices, 2, 20));
	}

	/* Unit 0: broadcast, no response. */
	ok = send_rtu_req(child.fd, 0, req, sizeof(req));
	ok = ok && recv_none_fd(child.fd, 120);
	/* Unit 1: valid edge. */
	if (ok)
	{
		ok = send_rtu_req(child.fd, 1, req, sizeof(req));
		ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 800);
		ok = ok && addr == 1 && get_be16(resp + 2) == 0x0001;
	}
	/* Unit 247: max valid RTU unit. */
	if (ok)
	{
		ok = send_rtu_req(child.fd, 247, req, sizeof(req));
		ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 800);
		ok = ok && addr == 247 && get_be16(resp + 2) == 0x00F7;
	}
	/* Unit 248: reserved in RTU; valid frame but no configured device -> no response. */
	if (ok)
	{
		ok = send_rtu_req(child.fd, 248, req, sizeof(req));
		ok = ok && recv_none_fd(child.fd, 120);
	}
	/* Server still healthy. */
	if (ok)
	{
		ok = send_rtu_req(child.fd, 1, req, sizeof(req));
		ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 800);
		ok = ok && addr == 1;
	}

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerReadWriteMultipleRegistersResponseMatchesPduShape)
{
	/* Same request/response shape as PDU test ReadWriteMultipleRegistersSuccessAndReadback. */
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x1234);
	rtu_child_t	child = { 0 };
	u8		req[] = {
		FC_READ_WRITE_MULTIPLE_REGISTERS,
		0x00, 0x00, 0x00, 0x02,	/* read addr 0, qty 2 */
		0x00, 0x00, 0x00, 0x02,	/* write addr 0, qty 2 */
		0x04,			/* byte count */
		0xAA, 0xAA, 0xBB, 0xBB,	/* write values */
	};
	u8		expected[] = {
		FC_READ_WRITE_MULTIPLE_REGISTERS,
		0x04,
		0xAA, 0xAA, 0xBB, 0xBB,
	};
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;

	ASSERT_EQ(dev != NULL);
	{
		point_t		p = { 0 };
		device_t*	devices[] = { dev };

		p.space = SPACE_HOLDING;
		p.address = 1;
		p.type = VT_U16;
		p.kind = POINT_STATIC;
		p.static_value = 0x5678;
		ASSERT_EQ(device_add_point(dev, &p) == NULL);
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));
	}

	ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
	ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 800));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(len == sizeof(expected));
	ASSERT_EQ(memcmp(resp, expected, sizeof(expected)) == 0);

	/* Verify write took effect via FC 03, same as PDU readback. */
	{
		u8	read_req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x02 };
		u8	read_expected[] = { FC_READ_HOLDING_REGISTERS, 0x04, 0xAA, 0xAA, 0xBB, 0xBB };

		ASSERT_EQ(send_rtu_req(child.fd, 1, read_req, sizeof(read_req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 800));
		ASSERT_EQ(addr == 1);
		ASSERT_EQ(len == sizeof(read_expected));
		ASSERT_EQ(memcmp(resp, read_expected, sizeof(read_expected)) == 0);
	}

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
}

TEST(RtuServerReadWriteMultipleRegistersSingleAndCoalesced)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x1234);
	rtu_child_t	child = { 0 };
	u8		pdu1[12] = { FC_READ_WRITE_MULTIPLE_REGISTERS };
	u8		pdu2[12] = { FC_READ_WRITE_MULTIPLE_REGISTERS };
	u8		wire[MODBUS_RTU_MAX_FRAME * 2];
	u8		buf[MODBUS_RTU_MAX_FRAME];
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	u32		wire_len = 0;
	u32		got = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	{
		point_t		p = { 0 };
		device_t*	devices[] = { dev };

		p.space = SPACE_HOLDING;
		p.address = 1;
		p.type = VT_U16;
		p.kind = POINT_STATIC;
		p.static_value = 0;
		ASSERT_EQ(device_add_point(dev, &p) == NULL);
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));
	}

	/* FC 23: read address 0, write address 1 = 0x002A. */
	put_be16(pdu1 + 1, 0);    /* read address */
	put_be16(pdu1 + 3, 1);    /* read quantity */
	put_be16(pdu1 + 5, 1);    /* write address */
	put_be16(pdu1 + 7, 1);    /* write quantity */
	pdu1[9] = 2;              /* byte count */
	put_be16(pdu1 + 10, 0x002A);

	/* FC 23: read address 1 (0x002A), write address 0 = 0x0BAD. */
	put_be16(pdu2 + 1, 1);
	put_be16(pdu2 + 3, 1);
	put_be16(pdu2 + 5, 0);
	put_be16(pdu2 + 7, 1);
	pdu2[9] = 2;
	put_be16(pdu2 + 10, 0x0BAD);

	/* Two coalesced FC 23 requests in one write. */
	wire_len  = modbus_rtu_encode(wire, 1, pdu1, sizeof(pdu1));
	wire_len += modbus_rtu_encode(wire + wire_len, 1, pdu2, sizeof(pdu2));
	ok = send_all_fd(child.fd, wire, wire_len);
	ASSERT_EQ(ok);

	ASSERT_EQ(recv_rtu_resp_buf(child.fd, buf, &got, &addr, resp, &len, 800));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(len == 4);
	ASSERT_EQ(resp[0] == FC_READ_WRITE_MULTIPLE_REGISTERS);
	ASSERT_EQ(resp[1] == 2);
	ASSERT_EQ(get_be16(resp + 2) == 0x1234);

	ASSERT_EQ(recv_rtu_resp_buf(child.fd, buf, &got, &addr, resp, &len, 800));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(len == 4);
	ASSERT_EQ(resp[0] == FC_READ_WRITE_MULTIPLE_REGISTERS);
	ASSERT_EQ(resp[1] == 2);
	ASSERT_EQ(get_be16(resp + 2) == 0x002A);
	ASSERT_EQ(recv_none_fd(child.fd, 100));

	/* Both writes took effect. */
	{
		u8	read_req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x02 };

		ASSERT_EQ(send_rtu_req(child.fd, 1, read_req, sizeof(read_req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 800));
		ASSERT_EQ(addr == 1 && resp[0] == FC_READ_HOLDING_REGISTERS);
		ASSERT_EQ(resp[1] == 4);
		ASSERT_EQ(get_be16(resp + 2) == 0x0BAD);
		ASSERT_EQ(get_be16(resp + 4) == 0x002A);
	}

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
}

TEST(RtuServerEofAfterRequestSendsResponse)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x1234);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));
	}

	ok = send_rtu_req(child.fd, 1, req, sizeof(req));
	if (ok) ok = shutdown(child.fd, SHUT_WR) == 0;
	if (ok) ok = recv_rtu_resp(child.fd, &addr, resp, &len, 1000);
	if (ok)
		ok = addr == 1 && len == 4 && resp[0] == FC_READ_HOLDING_REGISTERS &&
		     resp[1] == 2 && get_be16(resp + 2) == 0x1234;

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuServerEofAfterDelayedRequestSendsResponse)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x5678);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	u64		start = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	dev->fault.kind = FAULT_DELAY;
	dev->fault.delay_ms = 200;
	dev->fault.percent = 100;
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));
	}

	start = clock_now_ms();
	ok = send_rtu_req(child.fd, 1, req, sizeof(req));
	if (ok) ok = shutdown(child.fd, SHUT_WR) == 0;
	if (ok) ok = recv_rtu_resp(child.fd, &addr, resp, &len, 1500);
	if (ok) ok = (clock_now_ms() - start) >= 180;
	if (ok)
		ok = addr == 1 && len == 4 && resp[0] == FC_READ_HOLDING_REGISTERS &&
		     resp[1] == 2 && get_be16(resp + 2) == 0x5678;

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

static void
assert_fc16_split_answered(u32 split_after)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = device_new(arena, 0);
	rtu_child_t	child = { 0 };
	u8		pdu[6 + 8 * 2];
	u8		frame[MODBUS_RTU_MAX_FRAME];
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	u32		frame_len = 0;

	ASSERT_EQ(dev != NULL);
	dev->unit_id = 1;
	strcpy(dev->name, "rtu-fc16-split");
	for (u32 i = 0; i < 8; i++)
	{
		point_t p = { 0 };
		p.space = SPACE_HOLDING;
		p.address = i;
		p.type = VT_U16;
		p.kind = POINT_STATIC;
		p.static_value = (f64)i;
		ASSERT_EQ(device_add_point(dev, &p) == NULL);
	}

	pdu[0] = FC_WRITE_MULTIPLE_REGISTERS;
	put_be16(pdu + 1, 0);
	put_be16(pdu + 3, 8);
	pdu[5] = 16;
	for (u32 i = 0; i < 8; i++)
		put_be16(pdu + 6 + i * 2, (u16)(0x5000 + i));

	{
		device_t* devices[] = { dev };
		/* baud-derived auto mode; t3.5 at 9600 is ~4ms. */
		ASSERT_EQ(rtu_child_start_opts(&child, devices, 1, 0, 9600));
	}

	frame_len = modbus_rtu_encode(frame, 1, pdu, sizeof(pdu));
	ASSERT(frame_len > split_after);
	ASSERT_EQ(send_all_fd(child.fd, frame, split_after));
	usleep(6000); /* cross one t3.5 gap, within two-gap window */
	ASSERT_EQ(send_all_fd(child.fd, frame + split_after, frame_len - split_after));
	ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1500));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(len == 5);
	ASSERT_EQ(resp[0] == FC_WRITE_MULTIPLE_REGISTERS);
	ASSERT_EQ(get_be16(resp + 1) == 0);
	ASSERT_EQ(get_be16(resp + 3) == 8);

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
}

TEST(RtuServerBaudIncompleteFc16SplitAfter5Answered)
{
	/* Issue #49: FC16 needs 7 bytes before length is known; a 5-byte first
	 * chunk must still be kept in baud-derived auto mode. */
	assert_fc16_split_answered(5);
}

TEST(RtuServerBaudIncompleteFc16SplitAfter7Answered)
{
	assert_fc16_split_answered(7);
}

TEST(RtuServerBaudIncompleteFc16SplitAfter20Answered)
{
	assert_fc16_split_answered(20);
}

TEST(RtuServerBaudIncompleteFc08SplitAnswered)
{
	/* Issue #49: FC 08 request length cannot be derived from header alone;
	 * in baud-derived auto mode a split request must still be answered. */
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x1234);
	rtu_child_t	child = { 0 };
	u8		pdu[] = { FC_DIAGNOSTICS, 0x00, 0x00, 0x12, 0x34 };
	u8		frame[MODBUS_RTU_MAX_FRAME];
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	u32		frame_len = 0;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start_opts(&child, devices, 1, 0, 9600));
	}

	frame_len = modbus_rtu_encode(frame, 1, pdu, sizeof(pdu));
	ASSERT(frame_len > 4);
	ASSERT_EQ(send_all_fd(child.fd, frame, 4));
	usleep(6000);
	ASSERT_EQ(send_all_fd(child.fd, frame + 4, frame_len - 4));
	ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1500));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(len == sizeof(pdu));
	ASSERT_EQ(memcmp(resp, pdu, sizeof(pdu)) == 0);

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
}

TEST(RtuServerBaudTruncatedFrameDoesNotLoseValidRequest)
{
	/* Issue #49: a truncated frame followed by a valid request within two
	 * t3.5 gaps must not lose the valid request. */
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0xBEEF);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		good[MODBUS_RTU_MAX_FRAME];
	u8		trunc[] = { 0x01, FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	u32		good_len = 0;

	ASSERT_EQ(dev != NULL);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start_opts(&child, devices, 1, 0, 9600));
	}

	good_len = modbus_rtu_encode(good, 1, req, sizeof(req));
	ASSERT_EQ(send_all_fd(child.fd, trunc, sizeof(trunc)));
	usleep(10000); /* within two gaps: keep truncated bytes */
	ASSERT_EQ(send_all_fd(child.fd, good, good_len));
	ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1500));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(len == 4);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(get_be16(resp + 2) == 0xBEEF);

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
}
