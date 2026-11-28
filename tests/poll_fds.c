/* mogbus_server_poll_fds()/mogbus_poll_fds(): a host loop that waits with poll() on the reported
 * descriptors and then steps with a wait of 0 serves a client end to end. */
#define _GNU_SOURCE
#include "vtest.h"
#include "tests/tests_utils/test_case.h"
#include "mogbus.h"
#include "tests/tests_utils/test_support.h"
#include <poll.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>

enum
{
	TABLE_MAX = 64,
};

/* Host loop: ask what to wait on, poll() it, step without blocking. */
static void
host_wait_step(mogbus_t* ctx, mogbus_server_t* server)
{
	mogbus_poll_fd_t	fds[TABLE_MAX];
	struct pollfd		p[TABLE_MAX];
	size_t			n = 0;
	int			timeout = -1;

	if (server)
		ASSERT(mogbus_server_poll_fds(server, fds, TABLE_MAX, &n, &timeout) == MOGBUS_OK);
	else
		ASSERT(mogbus_poll_fds(ctx, fds, TABLE_MAX, &n, &timeout) == MOGBUS_OK);
	ASSERT(timeout >= 0 && timeout <= 1000);
	for (size_t i = 0; i < n; i++)
		p[i] = (struct pollfd){ .fd = (int)fds[i].fd,
					.events = (short)(((fds[i].events & MOGBUS_POLL_IN) ? POLLIN : 0) |
							  ((fds[i].events & MOGBUS_POLL_OUT) ? POLLOUT : 0)) };
	(void)poll(p, (nfds_t)n, timeout > 200 ? 200 : timeout);
	if (server)
		(void)mogbus_server_step(server, 0);
	else
		(void)mogbus_step(ctx, 0);
}

static bool
ask(int fd, mogbus_t* ctx, mogbus_server_t* server, uint16_t value)
{
	const u8	req[] = { 0, 1, 0, 0, 0, 6, 1, 3, 0, 0, 0, 1 };
	u8		rsp[11];

	if (!ts_send_socket(fd, req, sizeof(req)))
		return false;
	for (int i = 0; i < 50; i++)
	{
		struct pollfd p = { .fd = fd, .events = POLLIN };

		host_wait_step(ctx, server);
		if (poll(&p, 1, 0) > 0)
			break;
	}
	if (!ts_recv_exact(fd, rsp, sizeof(rsp), 1000))
		return false;
	return rsp[7] == 3 && rsp[9] == (value >> 8) && rsp[10] == (value & 0xFF);
}

static mogbus_t*
make(mogbus_server_t** servers, uint16_t* ports, int count)
{
	mogbus_t*		ctx;
	mogbus_device_t*	dev;
	mogbus_point_t		pt = { .space = MOGBUS_HOLDING, .address = 0, .type = MOGBUS_TYPE_U16, .value = 0x1234 };

	mogbus_create(&ctx);
	mogbus_add_device(ctx, 1, "d", &dev);
	mogbus_device_add_point(dev, &pt);
	for (int i = 0; i < count; i++)
	{
		mogbus_server_options_t o = mogbus_server_options_defaults();

		ports[i] = ts_free_port();
		o.port   = ports[i];
		mogbus_server_create(ctx, &o, &servers[i]);
	}
	return ctx;
}

TEST(PollFdsServerServesAClient)
{
	mogbus_server_t*	s[1];
	uint16_t		port[1];
	mogbus_t*		ctx = make(s, port, 1);
	mogbus_poll_fd_t	fds[TABLE_MAX];
	size_t			n = 0;
	int			timeout = -1;
	int			fd;

	ASSERT(s[0] != NULL);
	ASSERT(mogbus_server_poll_fds(s[0], fds, TABLE_MAX, &n, &timeout) == MOGBUS_OK);
	ASSERT(n == 1);
	ASSERT(fds[0].events == MOGBUS_POLL_IN);
	ASSERT(timeout > 0 && timeout <= 1000);
	ASSERT(mogbus_server_poll_fds(s[0], NULL, 0, &n, &timeout) == MOGBUS_ERR_INVALID);
	ASSERT(n == 1);
	fd = ts_connect_loopback(port[0], AF_INET, 1000);
	ASSERT(fd >= 0);
	ASSERT(ask(fd, ctx, s[0], 0x1234));
	ASSERT(mogbus_server_poll_fds(s[0], fds, TABLE_MAX, &n, &timeout) == MOGBUS_OK);
	ASSERT(n == 2);	/* listener and the client connection */
	close(fd);
	mogbus_destroy(ctx);
}

TEST(PollFdsContextServesTwoServers)
{
	mogbus_server_t*	s[2];
	uint16_t		port[2];
	mogbus_t*		ctx = make(s, port, 2);
	mogbus_poll_fd_t	fds[TABLE_MAX];
	size_t			n = 0;
	int			timeout = -1;
	int			a = ts_connect_loopback(port[0], AF_INET, 1000);
	int			b = ts_connect_loopback(port[1], AF_INET, 1000);

	ASSERT(a >= 0 && b >= 0);
	ASSERT(ask(a, ctx, NULL, 0x1234));
	ASSERT(ask(b, ctx, NULL, 0x1234));
	ASSERT(mogbus_poll_fds(ctx, fds, TABLE_MAX, &n, &timeout) == MOGBUS_OK);
	ASSERT(n == 4);
	ASSERT(mogbus_poll_fds(ctx, fds, 3, &n, &timeout) == MOGBUS_ERR_INVALID);
	ASSERT(n == 4);
	close(a);
	close(b);
	mogbus_destroy(ctx);
}

TEST(PollFdsStopMeansStepNow)
{
	mogbus_server_t*	s[1];
	uint16_t		port[1];
	mogbus_t*		ctx = make(s, port, 1);
	mogbus_poll_fd_t	fds[TABLE_MAX];
	size_t			n = 0;
	int			timeout = -1;

	mogbus_stop(ctx);
	ASSERT(mogbus_poll_fds(ctx, fds, TABLE_MAX, &n, &timeout) == MOGBUS_OK);
	ASSERT(timeout == 0);
	ASSERT(mogbus_step(ctx, 0) == MOGBUS_STEP_DONE);
	ASSERT(mogbus_poll_fds(ctx, fds, TABLE_MAX, &n, &timeout) == MOGBUS_OK);
	ASSERT(n == 0 && timeout == 0);
	ASSERT(mogbus_poll_fds(NULL, fds, TABLE_MAX, &n, &timeout) == MOGBUS_ERR_INVALID);
	mogbus_destroy(ctx);
}
