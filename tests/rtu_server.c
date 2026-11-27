#define _GNU_SOURCE
#include "vtest.h"
#include "modbus/bytes.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "net/rtu_framer.h"
#include "net/rtu_server.h"
#include "device/device.h"
#include "os/os_time.h"
#include "vlib/arena.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>
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

static void
set_fault(device_t* dev, fault_kind_t kind, u32 delay_ms, u32 percent)
{
	dev->faults.by_kind[kind] = (fault_t){
		.delay_ms = delay_ms,
		.percent  = percent,
	};
	dev->faults.present |= FAULT_BIT(kind);
}

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
		{ rtu_server_t srv; (void)rtu_server_run(&srv, &opts); }
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
		{ rtu_server_t srv; (void)rtu_server_run(&srv, &opts); }
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
	u64	deadline = os_now_ms() + timeout_ms;

	while (*got < MODBUS_RTU_MAX_FRAME)
	{
		struct pollfd	pfd = { .fd = fd, .events = POLLIN };
		u64		now = os_now_ms();
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

/* Unit 1 with `count` static holding registers 0..count-1 holding base+address. */
static device_t*
make_holding_device(arena_t* arena, const char* name, u32 count, u16 base)
{
	device_t* dev = device_new(arena, 0);

	if (!dev)
		return NULL;
	dev->unit_id = 1;
	strcpy(dev->name, name);
	for (u32 i = 0; i < count; i++)
	{
		point_t p = { 0 };

		p.space = SPACE_HOLDING;
		p.address = i;
		p.type = VT_U16;
		p.kind = POINT_STATIC;
		p.static_value = (f64)(base + i);
		if (device_add_point(dev, &p) != NULL)
			return NULL;
	}
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

TEST(RtuServerPendingDelayedResponseHangupNoSpin)
{
	/* Issue #19: peer closes while a delayed response is queued. Issue #40: the
	 * request and POLLHUP arrive in the same poll. Neither may busy-loop. */
	static const struct { u32 gap_ms; bool close_with_request; } cases[] = {
		{ 5, false },
		{ 20, true },
	};

	for (u32 c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
	{
		arena_t*	arena = ARENA_ALLOC();
		device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x3344);
		device_t*	devices[] = { dev };
		rtu_child_t	child = { 0 };
		u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
		u64		cpu_before = 0;
		u64		cpu_after = 0;
		bool		ok;

		ASSERT_EQ(dev != NULL);
		set_fault(dev, FAULT_DELAY, 500, 100);
		ASSERT_EQ(rtu_child_start(&child, devices, 1, cases[c].gap_ms));

		ok = send_rtu_req(child.fd, 1, req, sizeof(req));
		if (ok && cases[c].close_with_request)
		{
			close(child.fd);
			child.fd = -1;
		}
		if (ok) usleep(120000); /* let the server queue the delayed response */
		if (ok) ok = rtu_child_cpu_ms(child.pid, &cpu_before);
		if (ok && child.fd >= 0)
		{
			close(child.fd);
			child.fd = -1;
		}
		if (ok) usleep(350000);
		if (ok && !rtu_child_cpu_ms(child.pid, &cpu_after))
			cpu_after = cpu_before; /* process already exited; no spin */
		/* Tolerant for ASan/CI scheduling noise. */
		if (ok) ok = (cpu_after - cpu_before) < 200;
		if (!ok)
			fprintf(stderr, "hangup case %u spun\n", c);

		ASSERT_EQ(rtu_child_stop(&child));
		arena_release(arena);
		ASSERT_EQ(ok);
	}
}

TEST(RtuServerPrefixValidCrcDoesNotSplitLongFc16)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_holding_device(arena, "rtu-fc16", 123, 0x2000);
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
	ASSERT_EQ(found);

	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));
	}

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

TEST(RtuServerFragmentedFramesAcrossGaps)
{
	/* An 8-byte FC03 frame is written in chunks with a pause between them. A
	 * pause below the inter-frame gap keeps the frame whole; a longer one
	 * discards the first part, so nothing is answered and the server recovers. */
	static const struct {
		const char* name;
		u32	    frame_gap_ms;	/* 0: derive t3.5 from baud */
		u32	    baud;
		u8	    chunks[9];		/* sizes, 0-terminated */
		u32	    pause_us;
		bool	    answered;
	} cases[] = {
		{ "byte by byte within 80 ms gap",	80, 0,	   { 1, 1, 1, 1, 1, 1, 1, 1 }, 5000,	true },
		{ "halves across 30 ms gap",		30, 0,	   { 4, 4 },			120000, false },
		{ "9600 baud pairs within t3.5",	0,  9600,  { 2, 2, 2, 2 },		1000,	true },
		{ "9600 baud halves across t3.5",	0,  9600,  { 4, 4 },			20000,	false },
		{ "115200 baud halves 3 ms apart",	0,  115200, { 4, 4 },			3000,	true },
		{ "9600 baud 3+3+2 across two gaps",	0,  9600,  { 3, 3, 2 },			6000,	true },	/* issue #57 */
	};

	for (u32 c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
	{
		arena_t*	arena = ARENA_ALLOC();
		device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x55AA);
		device_t*	devices[] = { dev };
		rtu_child_t	child = { 0 };
		u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
		u8		frame[MODBUS_RTU_MAX_FRAME];
		u8		resp[MODBUS_MAX_PDU];
		u8		addr = 0;
		u32		len = 0;
		u32		flen = modbus_rtu_encode(frame, 1, req, sizeof(req));
		u32		off = 0;
		bool		ok = true;

		ASSERT_EQ(dev != NULL);
		ASSERT_EQ(rtu_child_start_opts(&child, devices, 1, cases[c].frame_gap_ms, cases[c].baud));
		for (u32 i = 0; cases[c].chunks[i] != 0 && ok; i++)
		{
			ok = send_all_fd(child.fd, frame + off, cases[c].chunks[i]);
			off += cases[c].chunks[i];
			if (cases[c].chunks[i + 1] != 0)
				usleep(cases[c].pause_us);
		}
		ok = ok && off == flen;
		if (cases[c].answered)
		{
			ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 1500);
			ok = ok && addr == 1 && len == 4 && get_be16(resp + 2) == 0x55AA;
			ok = ok && recv_none_fd(child.fd, 100);
		}
		else
		{
			ok = ok && recv_none_fd(child.fd, 150);
			ok = ok && send_rtu_req(child.fd, 1, req, sizeof(req));
			ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 800);
			ok = ok && addr == 1 && get_be16(resp + 2) == 0x55AA;
		}
		if (!ok)
			fprintf(stderr, "fragment case failed: %s\n", cases[c].name);

		ASSERT_EQ(rtu_child_stop(&child));
		arena_release(arena);
		ASSERT_EQ(ok);
	}
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
	device_t*	dev   = make_holding_device(arena, "rtu-max", 125, 0);
	rtu_child_t	child = { 0 };
	u8		pdu[6 + 123 * 2];
	u8		read_req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 125 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);

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
	set_fault(dev_delay, FAULT_DELAY, 250, 100);
	set_fault(dev_drop, FAULT_DROP, 0, 100);

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
		start = os_now_ms();
		ok = send_rtu_req(child.fd, 1, req, sizeof(req));
		ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 1200);
		ok = ok && (os_now_ms() - start) >= 200;
		ok = ok && addr == 1 && get_be16(resp + 2) == 0x1111;
	}

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuCorruptFaultSendsValidCrcWithWrongData)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x1234);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		diag[] = { FC_DIAGNOSTICS, 0x00, 0x0C, 0x00, 0x00 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;
	bool		ok = false;

	ASSERT_EQ(dev != NULL);
	set_fault(dev, FAULT_CORRUPT, 0, 100);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));
	}

	/* recv_rtu_resp accepts only frames whose CRC matches the returned PDU. */
	ok = send_rtu_req(child.fd, 1, req, sizeof(req));
	ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 800);
	ok = ok && addr == 1 && len == 4 &&
	     resp[0] == FC_READ_HOLDING_REGISTERS && resp[1] == (0x02 ^ 0xFF) &&
	     get_be16(resp + 2) == 0x1234;

	/* A corrupt response is a good frame, so it is not a communication error. */
	if (ok)
	{
		ok = send_rtu_req(child.fd, 1, diag, sizeof(diag));
		ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 800);
		ok = ok && addr == 1 && len == 5 && resp[0] == FC_DIAGNOSTICS &&
		     resp[1] == 0xFF && resp[2] == DIAG_SUB_RETURN_BUS_COMM_ERROR_COUNT &&
		     get_be16(resp + 3) == 0;
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
	/* One request per supported function code over the same connection. exp_len
	 * is the response PDU length, cmp_len how many leading bytes are compared with
	 * exp (or with req when exp is empty, i.e. an echo); check_reg >= 0 reads that
	 * holding register back and expects check_val. */
	static const struct {
		const char* name;
		u8	    req[12];
		u32	    req_len;
		u8	    exp[12];
		u32	    exp_len;
		u32	    cmp_len;
		int	    check_reg;
		u16	    check_val;
	} cases[] = {
		{ "FC01", { FC_READ_COILS, 0, 0, 0, 2 }, 5,
		  { FC_READ_COILS, 1, 0x01 }, 3, 3, -1, 0 },
		{ "FC02", { FC_READ_DISCRETE_INPUTS, 0, 0, 0, 1 }, 5,
		  { FC_READ_DISCRETE_INPUTS, 1, 0x01 }, 3, 3, -1, 0 },
		{ "FC03", { FC_READ_HOLDING_REGISTERS, 0, 0, 0, 1 }, 5,
		  { FC_READ_HOLDING_REGISTERS, 2, 0, 1 }, 4, 4, -1, 0 },
		{ "FC04", { FC_READ_INPUT_REGISTERS, 0, 0, 0, 2 }, 5,
		  { FC_READ_INPUT_REGISTERS, 4, 0x11, 0x22, 0x33, 0x44 }, 6, 6, -1, 0 },
		{ "FC05", { FC_WRITE_SINGLE_COIL, 0, 1, 0xFF, 0 }, 5, { 0 }, 5, 5, -1, 0 },
		{ "FC06", { FC_WRITE_SINGLE_REGISTER, 0, 2, 0, 0x2A }, 5, { 0 }, 5, 5, 2, 0x002A },
		{ "FC07", { FC_READ_EXCEPTION_STATUS }, 1,
		  { FC_READ_EXCEPTION_STATUS, 0 }, 2, 2, -1, 0 },
		{ "FC08", { FC_DIAGNOSTICS, 0, 0, 0x12, 0x34 }, 5, { 0 }, 5, 5, -1, 0 },
		{ "FC0B", { FC_GET_COM_EVENT_COUNTER }, 1,
		  { FC_GET_COM_EVENT_COUNTER, 0, 0 }, 5, 3, -1, 0 },
		{ "FC0C", { FC_GET_COM_EVENT_LOG }, 1,
		  { FC_GET_COM_EVENT_LOG, 6, 0, 0 }, 8, 4, -1, 0 },
		{ "FC0F", { FC_WRITE_MULTIPLE_COILS, 0, 0, 0, 2, 1, 2 }, 7, { 0 }, 5, 5, -1, 0 },
		{ "FC10", { FC_WRITE_MULTIPLE_REGISTERS, 0, 3, 0, 1, 2, 0, 0x99 }, 8, { 0 }, 5, 5, 3, 0x0099 },
		{ "FC11", { FC_REPORT_SERVER_ID }, 1,
		  { FC_REPORT_SERVER_ID, 2, 0x2A, 0xFF }, 4, 4, -1, 0 },
		{ "FC14", { FC_READ_FILE_RECORD, 7, 6, 0, 1, 0, 0, 0, 1 }, 9,
		  { FC_READ_FILE_RECORD, 4, 3, 6, 0xBE, 0xEF }, 6, 6, -1, 0 },
		{ "FC15", { FC_WRITE_FILE_RECORD, 9, 6, 0, 1, 0, 0, 0, 1, 0x12, 0x34 }, 11,
		  { 0 }, 11, 11, -1, 0 },
		{ "FC16", { FC_MASK_WRITE_REGISTER, 0, 8, 0, 0x0F, 0, 0xF0 }, 7, { 0 }, 7, 7, 8, 0x00FF },
		{ "FC17", { FC_READ_WRITE_MULTIPLE_REGISTERS, 0, 0, 0, 1, 0, 1, 0, 1, 2, 0, 0x77 }, 12,
		  { FC_READ_WRITE_MULTIPLE_REGISTERS, 2, 0, 1 }, 4, 4, 1, 0x0077 },
		{ "FC18 no fifo", { FC_READ_FIFO_QUEUE, 0, 0 }, 3,
		  { FC_READ_FIFO_QUEUE | 0x80, EXC_ILLEGAL_DATA_ADDRESS }, 2, 2, -1, 0 },
	};
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev = device_new(arena, 0);
	rtu_child_t	child = { 0 };
	u16		rec = 0xBEEF;

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
		ASSERT_EQ(device_add_file_record(dev, 1, 0, 1, &rec) == NULL);
	}
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));
	}

	for (u32 c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
	{
		u8	resp[MODBUS_MAX_PDU], addr = 0;
		u32	len = 0;
		bool	echo = cases[c].exp[0] == 0;
		bool	ok = send_rtu_req(child.fd, 1, cases[c].req, cases[c].req_len) &&
			     recv_rtu_resp(child.fd, &addr, resp, &len, 1200) &&
			     addr == 1 && len == cases[c].exp_len &&
			     memcmp(resp, echo ? cases[c].req : cases[c].exp, cases[c].cmp_len) == 0;

		if (ok && cases[c].check_reg >= 0)
			ok = rtu_expect_fc03(&child, 1, (u16)cases[c].check_reg, cases[c].check_val);
		if (!ok)
			fprintf(stderr, "function code case failed: %s\n", cases[c].name);
		ASSERT_EQ(ok);
	}

	{
		/* MEI 0x0E basic identification: three objects, exact framing. */
		u8 req[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x01, 0x00 };
		u8 resp[MODBUS_MAX_PDU], addr = 0;
		u32 len = 0;
		const u32 name_len = sizeof("mogbus") - 1;

		ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
		ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
		ASSERT_EQ(addr == 1 && len > 3);
		ASSERT_EQ(resp[0] == FC_ENCAPSULATED_INTERFACE_TRANSPORT && resp[1] == 0x0E);
		ASSERT_EQ(resp[2] == 0x01 && resp[3] == 0x81);
		ASSERT_EQ(resp[4] == 0x00 && resp[5] == 0x00 && resp[6] == 0x03);
		ASSERT_EQ(resp[7] == 0x00 && resp[8] == name_len);
		ASSERT_EQ(memcmp(resp + 9, "mogbus", name_len) == 0);
		ASSERT_EQ(resp[9 + name_len] == 0x01 && resp[10 + name_len] == name_len);
		ASSERT_EQ(memcmp(resp + 11 + name_len, "mogbus", name_len) == 0);
		ASSERT_EQ(resp[11 + 2 * name_len] == 0x02 && resp[12 + 2 * name_len] == 3);
		ASSERT_EQ(memcmp(resp + 13 + 2 * name_len, "1.0", 3) == 0);
	}

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
}

TEST(RtuServerMei13AndExtendedDeviceIdObjects)
{
	static const u8	obj[] = { 0x11, 0x22, 0x33, 0x44 };
	arena_t*	arena  = ARENA_ALLOC();
	device_t*	dev    = device_new(arena, 0);
	device_t*	devices[] = { dev };
	rtu_child_t	child  = { 0 };
	u8		pdu1[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D, 0x00, 0x00,
			   0x05, 0x60, 0x41, 0x00, 0x00, 0x00, 0x00, 0x02 };
	u8		pdu2[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x04, 0xA0 };
	u8		pdu3[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D, 0x01, 0x00,
			   0x05, 0x60, 0x41, 0x00, 0x00, 0x01, 0x00, 0x01, 0x77 };
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
	ASSERT_EQ(device_add_canopen_object(dev, 5, 0x6041, 0, sizeof(obj), obj) == NULL);
	ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));

	/* Both frames in one write: the framer derives the variable MEI 0x0D
	 * length from the count field before splitting the coalesced write. */
	f1 = modbus_rtu_encode(frame1, 1, pdu1, sizeof(pdu1));
	f2 = modbus_rtu_encode(frame2, 1, pdu2, sizeof(pdu2));
	ASSERT_EQ(f1 > 0 && f2 > 0);
	memcpy(both, frame1, f1);
	memcpy(both + f1, frame2, f2);
	ASSERT_EQ(send_all_fd(child.fd, both, f1 + f2));

	ASSERT_EQ(recv_rtu_resp_buf(child.fd, buf, &got, &addr, resp, &len, 1200));
	ASSERT_EQ(addr == 1 && len == 14);
	ASSERT_EQ(resp[0] == FC_ENCAPSULATED_INTERFACE_TRANSPORT && resp[1] == 0x0D);
	ASSERT_EQ(resp[10] == 0x00 && resp[11] == 0x02);
	ASSERT_EQ(resp[12] == 0x11 && resp[13] == 0x22);

	ASSERT_EQ(recv_rtu_resp_buf(child.fd, buf, &got, &addr, resp, &len, 1200));
	ASSERT_EQ(addr == 1 && len == 15);
	ASSERT_EQ(resp[0] == FC_ENCAPSULATED_INTERFACE_TRANSPORT && resp[1] == 0x0E);
	ASSERT_EQ(resp[2] == 0x04 && resp[3] == 0x83 && resp[6] == 0x01);
	ASSERT_EQ(resp[7] == 0xA0 && resp[8] == 6 && memcmp(resp + 9, "Second", 6) == 0);

	ASSERT_EQ(send_rtu_req(child.fd, 1, pdu3, sizeof(pdu3)));
	ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1200));
	ASSERT_EQ(addr == 1 && len == 12);
	ASSERT_EQ(resp[0] == FC_ENCAPSULATED_INTERFACE_TRANSPORT && resp[1] == 0x0D);
	ASSERT_EQ(resp[2] == 0x01 && resp[10] == 0x00 && resp[11] == 0x00);

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

TEST(RtuDiagnosticsListenOnlySuppressesResponsesUntilRestart)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x1234);
	rtu_child_t	child = { 0 };
	u8		req[5];
	u8		resp[MODBUS_MAX_PDU];
	u8		addr  = 0;
	u32		len   = 0;
	bool		ok    = false;

	ASSERT_EQ(dev != NULL);
	ASSERT_EQ(rtu_child_start(&child, &dev, 1, 20));

	req[0] = FC_READ_HOLDING_REGISTERS;
	put_be16(req + 1, 0);
	put_be16(req + 3, 1);
	ok = send_rtu_req(child.fd, 1, req, sizeof(req));
	ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 800);
	ok = ok && addr == 1 && get_be16(resp + 2) == 0x1234;

	/* Force Listen Only is itself answered with silence. */
	if (ok)
	{
		req[0] = FC_DIAGNOSTICS;
		put_be16(req + 1, DIAG_SUB_LISTEN_ONLY_MODE);
		put_be16(req + 3, 0x0000);
		ok = send_rtu_req(child.fd, 1, req, sizeof(req));
		ok = ok && recv_none_fd(child.fd, 150);
	}
	if (ok)
	{
		req[0] = FC_READ_HOLDING_REGISTERS;
		ok = send_rtu_req(child.fd, 1, req, sizeof(req));
		ok = ok && recv_none_fd(child.fd, 150);
	}

	/* Restart is processed while listen-only, but is not answered either. */
	if (ok)
	{
		req[0] = FC_DIAGNOSTICS;
		put_be16(req + 1, DIAG_SUB_RESTART_COMMUNICATIONS);
		put_be16(req + 3, 0x0000);
		ok = send_rtu_req(child.fd, 1, req, sizeof(req));
		ok = ok && recv_none_fd(child.fd, 150);
	}
	if (ok)
	{
		req[0] = FC_READ_HOLDING_REGISTERS;
		ok = send_rtu_req(child.fd, 1, req, sizeof(req));
		ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 800);
		ok = ok && addr == 1 && get_be16(resp + 2) == 0x1234;
	}

	/* FF00 also exits listen-only and clears the counters. */
	if (ok)
	{
		req[0] = FC_DIAGNOSTICS;
		put_be16(req + 1, DIAG_SUB_LISTEN_ONLY_MODE);
		put_be16(req + 3, 0x0000);
		ok = send_rtu_req(child.fd, 1, req, sizeof(req));
		ok = ok && recv_none_fd(child.fd, 150);
	}
	if (ok)
	{
		req[0] = FC_DIAGNOSTICS;
		put_be16(req + 1, DIAG_SUB_RESTART_COMMUNICATIONS);
		put_be16(req + 3, 0xFF00);
		ok = send_rtu_req(child.fd, 1, req, sizeof(req));
		ok = ok && recv_none_fd(child.fd, 150);
	}
	if (ok)
	{
		req[0] = FC_DIAGNOSTICS;
		put_be16(req + 1, DIAG_SUB_RETURN_SLAVE_NO_RESPONSE_COUNT);
		put_be16(req + 3, 0x0000);
		ok = send_rtu_req(child.fd, 1, req, sizeof(req));
		ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 800);
		ok = ok && addr == 1 && len == 5 && get_be16(resp + 3) == 1;
	}
	if (ok)
	{
		req[0] = FC_READ_HOLDING_REGISTERS;
		put_be16(req + 1, 0);
		put_be16(req + 3, 1);
		ok = send_rtu_req(child.fd, 1, req, sizeof(req));
		ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 800);
		ok = ok && addr == 1 && get_be16(resp + 2) == 0x1234;
	}

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
	ASSERT_EQ(ok);
}

TEST(RtuBadCrcIncrementsBusCommErrorForEveryDevice)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev1  = make_device(arena, 1, SPACE_HOLDING, 0, 0x1234);
	device_t*	dev2  = make_device(arena, 2, SPACE_HOLDING, 0, 0x5678);
	device_t*	devices[] = { dev1, dev2 };
	rtu_child_t	child = { 0 };
	u8		frame[MODBUS_RTU_MAX_FRAME];
	u8		pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		req[5];
	u8		resp[MODBUS_MAX_PDU];
	u8		addr  = 0;
	u32		frame_len = 0;
	u32		len   = 0;
	bool		ok    = false;

	ASSERT_EQ(dev1 != NULL && dev2 != NULL);
	ASSERT_EQ(rtu_child_start(&child, devices, 2, 20));

	frame_len = modbus_rtu_encode(frame, 1, pdu, sizeof(pdu));
	ASSERT_EQ(frame_len == sizeof(pdu) + 3);
	frame[frame_len - 2] ^= 0xFF; /* CRC low byte: a complete but corrupt frame */
	ok = send_all_fd(child.fd, frame, frame_len);
	usleep(100000); /* let the server process the inter-frame gap */

	for (u32 i = 0; ok && i < 2; i++)
	{
		req[0] = FC_DIAGNOSTICS;
		put_be16(req + 1, DIAG_SUB_RETURN_BUS_COMM_ERROR_COUNT);
		put_be16(req + 3, 0x0000);
		ok = send_rtu_req(child.fd, (u8)(i + 1), req, sizeof(req));
		ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 800);
		ok = ok && addr == (u8)(i + 1) && len == 5 && get_be16(resp + 3) == 1;
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
	/* The peer half-closes right after the request: the immediate and the
	 * delayed (fault) response must both still be delivered. */
	static const struct { u32 delay_ms; u16 value; u32 min_elapsed_ms; } cases[] = {
		{ 0, 0x1234, 0 },
		{ 200, 0x5678, 180 },
	};

	for (u32 c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
	{
		arena_t*	arena = ARENA_ALLOC();
		device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, cases[c].value);
		rtu_child_t	child = { 0 };
		u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
		u8		resp[MODBUS_MAX_PDU];
		u8		addr = 0;
		u32		len = 0;
		u64		start = 0;
		bool		ok = false;

		ASSERT_EQ(dev != NULL);
		if (cases[c].delay_ms > 0)
			set_fault(dev, FAULT_DELAY, cases[c].delay_ms, 100);
		{
			device_t* devices[] = { dev };
			ASSERT_EQ(rtu_child_start(&child, devices, 1, 20));
		}

		start = os_now_ms();
		ok = send_rtu_req(child.fd, 1, req, sizeof(req));
		if (ok) ok = shutdown(child.fd, SHUT_WR) == 0;
		if (ok) ok = recv_rtu_resp(child.fd, &addr, resp, &len, 1500);
		if (ok) ok = (os_now_ms() - start) >= cases[c].min_elapsed_ms;
		if (ok)
			ok = addr == 1 && len == 4 && resp[0] == FC_READ_HOLDING_REGISTERS &&
			     resp[1] == 2 && get_be16(resp + 2) == cases[c].value;
		if (!ok)
			fprintf(stderr, "EOF case with delay %u ms failed\n", cases[c].delay_ms);

		ASSERT_EQ(rtu_child_stop(&child));
		arena_release(arena);
		ASSERT_EQ(ok);
	}
}

TEST(RtuServerBaudIncompleteFc16SplitIsAnswered)
{
	/* Issue #49: FC16 needs 7 bytes before length is known; a 5-byte first
	 * chunk must still be kept in baud-derived auto mode. */
	static const u32 splits[] = { 5, 7, 20 };

	for (u32 c = 0; c < sizeof(splits) / sizeof(splits[0]); c++)
	{
		u32		split_after = splits[c];
		arena_t*	arena = ARENA_ALLOC();
		device_t*	dev   = make_holding_device(arena, "rtu-fc16-split", 8, 0);
		rtu_child_t	child = { 0 };
		u8		pdu[6 + 8 * 2];
		u8		frame[MODBUS_RTU_MAX_FRAME];
		u8		resp[MODBUS_MAX_PDU];
		u8		addr = 0;
		u32		len = 0;
		u32		frame_len = 0;
		bool		ok;

		ASSERT_EQ(dev != NULL);
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
		ok = send_all_fd(child.fd, frame, split_after);
		usleep(6000); /* cross one t3.5 gap, within two-gap window */
		ok = ok && send_all_fd(child.fd, frame + split_after, frame_len - split_after);
		ok = ok && recv_rtu_resp(child.fd, &addr, resp, &len, 1500);
		ok = ok && addr == 1 && len == 5 && resp[0] == FC_WRITE_MULTIPLE_REGISTERS &&
		     get_be16(resp + 1) == 0 && get_be16(resp + 3) == 8;
		if (!ok)
			fprintf(stderr, "FC16 split after %u bytes failed\n", split_after);

		ASSERT_EQ(rtu_child_stop(&child));
		arena_release(arena);
		ASSERT_EQ(ok);
	}
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
	/* Time the gap from when the server has read the truncated bytes: if it
	 * read both writes at once (busy machine) there would be no silence at all. */
	{
		int unread = 1;

		for (int i = 0; i < 1000 && unread > 0; i++)
		{
			ASSERT_EQ(ioctl(child.fd, SIOCOUTQ, &unread) == 0);
			if (unread > 0)
				usleep(1000);
		}
		ASSERT_EQ(unread == 0);
	}
	usleep(7000); /* between one and two 5 ms gaps: truncated bytes are kept */
	ASSERT_EQ(send_all_fd(child.fd, good, good_len));
	ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 1500));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(len == 4);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(get_be16(resp + 2) == 0xBEEF);

	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
}

TEST(RtuServerBroadcastDoesNotConsumeFaultScheduleStep)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena, 1, SPACE_HOLDING, 0, 0x0001);
	rtu_child_t	child = { 0 };
	u8		req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len = 0;

	ASSERT_EQ(dev != NULL);
	dev->faults.by_kind[FAULT_EXCEPTION] = (fault_t){
		.exception_code = EXC_SLAVE_DEVICE_BUSY,
		.schedule_count = 1,
		.schedule_id    = 1,
		.schedule       = { { .after_ms = 0, .count = 1 } },
	};
	dev->faults.present |= FAULT_BIT(FAULT_EXCEPTION);
	{
		device_t* devices[] = { dev };
		ASSERT_EQ(rtu_child_start_opts_flags(&child, devices, 1, 20, 0, false, true));
	}
	ASSERT_EQ(send_rtu_req(child.fd, 0, req, sizeof(req)));
	ASSERT_EQ(recv_none_fd(child.fd, 120));
	ASSERT_EQ(send_rtu_req(child.fd, 1, req, sizeof(req)));
	ASSERT_EQ(recv_rtu_resp(child.fd, &addr, resp, &len, 800));
	ASSERT_EQ(addr == 1 && len == 2);
	ASSERT_EQ(resp[0] == 0x83 && resp[1] == 0x06);
	ASSERT_EQ(rtu_child_stop(&child));
	arena_release(arena);
}
