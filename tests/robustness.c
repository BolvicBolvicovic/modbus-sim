/* Malformed and fragmented frames against real servers, run in-process through the public API:
 * every transport must survive garbage and still answer valid requests afterwards. Garbage is
 * deterministic (fixed-seed LCG), so a failure reproduces. */
#define _GNU_SOURCE
#include "vtest.h"
#include "mogbus.h"
#include "modbus/ascii.h"
#include "modbus/mbap.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "os/os_time.h"
#include "tests/tests_utils/test_support.h"
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const u8 READ_HOLDING[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };

static u32 g_lcg = 12345;

static u8
next_byte(void)
{
	g_lcg = g_lcg * 1103515245u + 12345u;
	return (u8)(g_lcg >> 16);
}

/* Garbage biased towards bytes that start frames, so it reaches deep parser states. */
static void
fill_garbage(u8* buf, u32 len)
{
	static const u8 interesting[] = { ':', '\r', '\n', 0x00, 0x01, 0x03, 0xFF, 0x2B };

	for (u32 i = 0; i < len; i++)
	{
		u8 b = next_byte();

		buf[i] = (b & 3) == 0 ? interesting[b % sizeof(interesting)] : next_byte();
	}
}

typedef struct fixture_t fixture_t;
struct fixture_t
{
	mogbus_t*		ctx;
	mogbus_server_t*	server;
	mogbus_mode_t		mode;
	u16			port;
};

static bool
fixture_start(fixture_t* f, mogbus_mode_t mode)
{
	mogbus_point_t		p = { .space = MOGBUS_HOLDING, .address = 0, .type = MOGBUS_TYPE_U16,
				      .kind = MOGBUS_POINT_STATIC, .value = 0x1234 };
	mogbus_server_options_t	o = mogbus_server_options_defaults();
	mogbus_device_t*	dev;
	char			err[256];

	f->ctx  = mogbus_create();
	f->mode = mode;
	f->port = ts_free_port();
	dev = f->ctx ? mogbus_add_device(f->ctx, 1, "robust", err, sizeof(err)) : NULL;
	if (!dev || !mogbus_device_add_point(dev, &p, err, sizeof(err)))
		return false;
	o.mode = mode;
	o.port = f->port;
	f->server = mogbus_server_create(f->ctx, &o, err, sizeof(err), NULL);
	return f->server != NULL;
}

static void
fixture_stop(fixture_t* f)
{
	mogbus_server_close(f->server);
	mogbus_destroy(f->ctx);
}

static bool
is_datagram(mogbus_mode_t mode)
{
	return mode == MOGBUS_MODE_UDP || mode == MOGBUS_MODE_RTU_UDP || mode == MOGBUS_MODE_ASCII_UDP;
}

static int
client_open(const fixture_t* f)
{
	return is_datagram(f->mode) ? ts_udp_loopback(f->port, AF_INET, 1000)
				    : ts_connect_loopback(f->port, AF_INET, 1000);
}

/* Steps the server for `ms` milliseconds. */
static void
step_for(fixture_t* f, u32 ms)
{
	u64 until = os_now_ms() + ms;

	while (os_now_ms() < until)
		(void)mogbus_server_step(f->server, 1);
}

/* The read-holding request in the mode's framing. */
static u32
encode_request(mogbus_mode_t mode, u8* out)
{
	switch (mode)
	{
		case MOGBUS_MODE_RTU_TCP:
		case MOGBUS_MODE_RTU_UDP:	return modbus_rtu_encode(out, 1, READ_HOLDING, sizeof(READ_HOLDING));
		case MOGBUS_MODE_ASCII_TCP:
		case MOGBUS_MODE_ASCII_UDP:	return modbus_ascii_encode(out, 1, READ_HOLDING, sizeof(READ_HOLDING));
		default:			return ts_mbap_encode(out, 1, 0x4242, READ_HOLDING, sizeof(READ_HOLDING));
	}
}

/* Reads one reply while stepping the server and checks it carries register value 0x1234. */
static bool
expect_reply(fixture_t* f, int fd)
{
	u8	buf[MODBUS_ASCII_MAX_FRAME];
	u32	len = 0;
	u64	deadline = os_now_ms() + 2000;

	while (os_now_ms() < deadline && len < sizeof(buf))
	{
		struct pollfd	pfd = { .fd = fd, .events = POLLIN };
		ssize_t		got;
		u8		decoded[MODBUS_MAX_PDU + 2];
		u8		addr = 0;
		const u8*	pdu = NULL;
		u32		pdu_len = 0;
		u32		frame_len = 0;

		(void)mogbus_server_step(f->server, 1);
		if (poll(&pfd, 1, 0) <= 0)
			continue;
		got = recv(fd, buf + len, sizeof(buf) - len, 0);
		if (got <= 0)
			return false;
		len += (u32)got;
		switch (f->mode)
		{
			case MOGBUS_MODE_RTU_TCP:
			case MOGBUS_MODE_RTU_UDP:
				if (!modbus_rtu_decode(buf, len, &addr, &pdu, &pdu_len))
					continue;
				break;
			case MOGBUS_MODE_ASCII_TCP:
			case MOGBUS_MODE_ASCII_UDP:
				if (modbus_ascii_scan(buf, len, &frame_len) != MODBUS_ASCII_SCAN_FRAME ||
				    !modbus_ascii_decode(buf, frame_len, decoded, &addr, &pdu, &pdu_len))
					continue;
				break;
			default:
				if (len < MBAP_HEADER_SIZE + 4)
					continue;
				addr    = buf[6];
				pdu     = buf + MBAP_HEADER_SIZE;
				pdu_len = len - MBAP_HEADER_SIZE;
				break;
		}
		return addr == 1 && pdu_len == 4 && pdu[0] == FC_READ_HOLDING_REGISTERS &&
		       pdu[2] == 0x12 && pdu[3] == 0x34;
	}
	return false;
}

/* A valid request on a fresh client gets the right answer. */
static bool
still_answers(fixture_t* f)
{
	u8	req[MODBUS_ASCII_MAX_FRAME];
	u32	len = encode_request(f->mode, req);
	int	fd = client_open(f);
	bool	ok;

	step_for(f, 30);	/* let RTU see a frame gap and the server accept */
	ok = fd >= 0 && send(fd, req, len, MSG_NOSIGNAL) == (ssize_t)len && expect_reply(f, fd);
	if (fd >= 0)
		close(fd);
	return ok;
}

static bool
fragmented_request_is_answered(mogbus_mode_t mode)
{
	fixture_t	f;
	u8		req[MODBUS_ASCII_MAX_FRAME];
	u32		len;
	int		fd;
	bool		ok;

	if (!fixture_start(&f, mode))
		return false;
	len = encode_request(mode, req);
	fd  = client_open(&f);
	ok  = fd >= 0;
	step_for(&f, 10);
	/* One byte per send, the server stepping in between (well under the RTU frame gap). */
	for (u32 i = 0; ok && i < len; i++)
	{
		ok = send(fd, req + i, 1, MSG_NOSIGNAL) == 1;
		(void)mogbus_server_step(f.server, 0);
	}
	ok = ok && expect_reply(&f, fd);
	if (fd >= 0)
		close(fd);
	fixture_stop(&f);
	return ok;
}

static bool
survives_stream_garbage(mogbus_mode_t mode)
{
	fixture_t	f;
	bool		ok;

	if (!fixture_start(&f, mode))
		return false;
	ok = true;
	for (u32 round = 0; ok && round < 20; round++)
	{
		u8	garbage[700];
		u32	len = 1 + next_byte() % sizeof(garbage);
		int	fd = client_open(&f);

		ok = fd >= 0;
		fill_garbage(garbage, len);
		/* Random chunks; a send may fail once MBAP has closed the connection, which is fine. */
		for (u32 sent = 0; ok && sent < len;)
		{
			u32 chunk = 1 + next_byte() % 64;

			if (chunk > len - sent)
				chunk = len - sent;
			if (send(fd, garbage + sent, chunk, MSG_NOSIGNAL) != (ssize_t)chunk)
				break;
			sent += chunk;
			(void)mogbus_server_step(f.server, 0);
		}
		if (fd >= 0)
			close(fd);
		step_for(&f, 5);
		ok = ok && still_answers(&f);
	}
	fixture_stop(&f);
	return ok;
}

static bool
survives_datagram_garbage(mogbus_mode_t mode)
{
	fixture_t	f;
	u8		req[MODBUS_ASCII_MAX_FRAME];
	u8		silent[64];
	u32		len;
	int		fd;
	bool		ok;

	if (!fixture_start(&f, mode))
		return false;
	fd = client_open(&f);
	ok = fd >= 0;
	for (u32 i = 0; ok && i < 200; i++)
	{
		u8	garbage[600];
		u32	glen = 1 + next_byte() % sizeof(garbage);

		fill_garbage(garbage, glen);
		ok = send(fd, garbage, glen, MSG_NOSIGNAL) == (ssize_t)glen;
		(void)mogbus_server_step(f.server, 0);
	}
	/* A valid frame split over two datagrams is two malformed datagrams: no reply at all. */
	len = encode_request(mode, req);
	ok = ok && send(fd, req, len / 2, MSG_NOSIGNAL) == (ssize_t)(len / 2);
	ok = ok && send(fd, req + len / 2, len - len / 2, MSG_NOSIGNAL) == (ssize_t)(len - len / 2);
	step_for(&f, 50);
	ok = ok && recv(fd, silent, sizeof(silent), MSG_DONTWAIT) < 0;
	if (fd >= 0)
		close(fd);
	ok = ok && still_answers(&f);
	fixture_stop(&f);
	return ok;
}

static const mogbus_mode_t STREAM_MODES[] = { MOGBUS_MODE_TCP, MOGBUS_MODE_RTU_TCP, MOGBUS_MODE_ASCII_TCP };
static const mogbus_mode_t DATAGRAM_MODES[] = { MOGBUS_MODE_UDP, MOGBUS_MODE_RTU_UDP, MOGBUS_MODE_ASCII_UDP };

TEST(RobustStreamTransportsAnswerAByteAtATimeRequest)
{
	for (u32 i = 0; i < sizeof(STREAM_MODES) / sizeof(STREAM_MODES[0]); i++)
	{
		bool ok = fragmented_request_is_answered(STREAM_MODES[i]);

		if (!ok)
			fprintf(stderr, "mode %d failed\n", (int)STREAM_MODES[i]);
		ASSERT_EQ(ok);
	}
}

TEST(RobustEveryTransportSurvivesGarbage)
{
	for (u32 i = 0; i < 6; i++)
	{
		bool stream = i < 3;
		mogbus_mode_t mode = stream ? STREAM_MODES[i] : DATAGRAM_MODES[i - 3];
		bool ok = stream ? survives_stream_garbage(mode) : survives_datagram_garbage(mode);

		if (!ok)
			fprintf(stderr, "mode %d failed\n", (int)mode);
		ASSERT_EQ(ok);
	}
}
