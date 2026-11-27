/**
 * @file
 * @brief Embeds mogbus in a host program: one device built in code, a Modbus TCP server on a
 *        free loopback port, and a raw MBAP client in the same process.
 *
 * Only "mogbus.h" is included from the library. Exit status 0 means the register read back the
 * expected value.
 */
#include "mogbus.h"
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/** @brief Asks the kernel for a free loopback port (the port is reused right away). */
static uint16_t
free_port(void)
{
	struct sockaddr_in	addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t		len  = sizeof(addr);
	int			fd   = socket(AF_INET, SOCK_STREAM, 0);
	uint16_t		port = 0;

	if (fd < 0)
		return 0;
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0 &&
	    getsockname(fd, (struct sockaddr*)&addr, &len) == 0)
		port = ntohs(addr.sin_port);
	close(fd);
	return port;
}

int
main(void)
{
	mogbus_t*	ctx = NULL;
	mogbus_device_t* dev      = NULL;
	mogbus_server_t* server   = NULL;
	mogbus_server_options_t options = mogbus_server_options_defaults();
	mogbus_point_t	point     = { .space = MOGBUS_HOLDING, .address = 10, .type = MOGBUS_TYPE_U16, .value = 1234 };
	int		client    = -1;
	int		status    = 1;

	printf("mogbus %s\n", mogbus_version());
	if (mogbus_create(&ctx) != MOGBUS_OK)
	{
		fprintf(stderr, "%s\n", mogbus_last_error(NULL));
		return 1;
	}
	if (mogbus_add_device(ctx, 1, "embedded", &dev) != MOGBUS_OK ||
	    mogbus_device_add_point(dev, &point) != MOGBUS_OK)
	{
		fprintf(stderr, "device: %s\n", mogbus_last_error(ctx));
		goto done;
	}

	options.port = free_port();
	if (mogbus_server_create(ctx, &options, &server) != MOGBUS_OK)
	{
		fprintf(stderr, "server: %s\n", mogbus_last_error(ctx));
		goto done;
	}

	{
		struct sockaddr_in addr = {
			.sin_family      = AF_INET,
			.sin_port        = htons(options.port),
			.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
		};
		/* MBAP: transaction 1, protocol 0, length 6, unit 1; FC 03, address 10, 1 register. */
		static const uint8_t request[] = { 0, 1, 0, 0, 0, 6, 1, 0x03, 0, 10, 0, 1 };
		uint8_t		reply[11];
		size_t		got = 0;

		client = socket(AF_INET, SOCK_STREAM, 0);
		if (client < 0 || connect(client, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
		    write(client, request, sizeof(request)) != (ssize_t)sizeof(request))
		{
			perror("client");
			goto done;
		}

		/* The host owns the loop: serve between client I/O, for up to ~2 s. */
		for (int i = 0; i < 2000 && got < sizeof(reply); i++)
		{
			struct pollfd pfd = { .fd = client, .events = POLLIN };

			if (mogbus_server_step(server, 1) != MOGBUS_STEP_CONTINUE)
				break;
			if (poll(&pfd, 1, 0) > 0)
			{
				ssize_t n = read(client, reply + got, sizeof(reply) - got);

				if (n <= 0)
					break;
				got += (size_t)n;
			}
		}
		/* tid 1, length 5, unit 1, FC 03, 2 data bytes: 1234 = 0x04D2. */
		if (got != sizeof(reply) || reply[1] != 1 || reply[7] != 0x03 ||
		    reply[8] != 2 || reply[9] != 0x04 || reply[10] != 0xD2)
		{
			fprintf(stderr, "unexpected reply (%zu bytes)\n", got);
			goto done;
		}
	}

	/* The host can also read and change values directly. */
	{
		double value = 0;

		if (mogbus_device_get_value(dev, MOGBUS_HOLDING, 10, &value) != MOGBUS_OK || value != 1234)
			goto done;
	}
	puts("read holding register 10 = 1234 over TCP");
	status = 0;

done:
	if (client >= 0)
		close(client);
	mogbus_server_close(server);
	mogbus_destroy(ctx);
	return status;
}
