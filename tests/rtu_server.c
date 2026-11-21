#define _GNU_SOURCE
#include "vtest.h"
#include "modbus/bytes.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
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
rtu_child_start(rtu_child_t* child, device_t** devices, u32 device_count, u32 frame_gap_ms)
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
recv_rtu_resp(int fd, u8* addr, u8* pdu, u32* pdu_len, u32 timeout_ms)
{
	u8	buf[MODBUS_RTU_MAX_FRAME];
	u32	got = 0;
	u64	deadline = clock_now_ms() + timeout_ms;

	while (got < sizeof(buf))
	{
		struct pollfd	pfd = { .fd = fd, .events = POLLIN };
		u64		now = clock_now_ms();
		int		wait_ms;
		ssize_t		n;
		const u8*	out_pdu = NULL;
		u32		out_len = 0;
		u8		out_addr = 0;

		if (now >= deadline)
			return false;
		wait_ms = (int)(deadline - now);
		if (poll(&pfd, 1, wait_ms) <= 0)
			continue;
		if (!(pfd.revents & POLLIN))
		{
			if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
				return false;
			continue;
		}
		n = read(fd, buf + got, sizeof(buf) - got);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return false;
		got += (u32)n;
		if (got >= 4 && modbus_rtu_decode(buf, got, &out_addr, &out_pdu, &out_len))
		{
			*addr = out_addr;
			*pdu_len = out_len;
			memcpy(pdu, out_pdu, out_len);
			return true;
		}
	}
	return false;
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
		u64	cpu_before = 0;
		u64	cpu_after = 0;

		ok = send_all_fd(child.fd, junk, junk_len);
		ok = ok && recv_none_fd(child.fd, 60);
		/* After gap elapses, server must drop short junk and return to poll(). */
		if (ok) ok = rtu_child_cpu_ms(child.pid, &cpu_before);
		if (ok) usleep(200000);
		if (ok) ok = rtu_child_cpu_ms(child.pid, &cpu_after);
		if (ok) ok = (cpu_after - cpu_before) < 150;

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
