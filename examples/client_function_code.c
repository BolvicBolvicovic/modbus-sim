/**
 * @file
 * @brief A vendor function code (0x41) answered by a callback, and a mogbus client that calls it.
 *
 * The server runs on its own thread; the client only talks to it over a TCP socket, as it would to
 * a real device. Only "mogbus.h" is included from the library. Exit status 0 means both calls
 * behaved as expected.
 */
#include "mogbus.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

/** @brief The vendor function code of this example. */
enum { STATUS_CODE = 0x41 };

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

/* Request 41 01 answers with the live value of holding register 0; anything else is exception 03. */
static int
status(void* user, mogbus_device_t* dev, uint8_t unit_id, uint8_t code, const uint8_t* data, size_t len,
       uint8_t* reply, size_t reply_cap, size_t* reply_len)
{
	double value = 0;

	(void)user; (void)unit_id; (void)code; (void)reply_cap;
	if (len != 1 || data[0] != 0x01)
		return 3;
	if (!mogbus_device_get_value(dev, MOGBUS_HOLDING, 0, &value))
		return 4;
	reply[0]   = (uint8_t)((unsigned)value >> 8);
	reply[1]   = (uint8_t)value;
	*reply_len = 2;
	return 0;
}

static void*
serve(void* server)
{
	(void)mogbus_server_run(server);
	return NULL;
}

int
main(void)
{
	mogbus_t*		ctx     = mogbus_create();
	mogbus_device_t*	dev     = ctx ? mogbus_add_device(ctx, 1, "meter") : NULL;
	mogbus_point_t		point   = { .space = MOGBUS_HOLDING, .address = 0, .type = MOGBUS_TYPE_U16, .value = 1234 };
	mogbus_server_options_t	so      = mogbus_server_options_defaults();
	mogbus_client_options_t	co      = mogbus_client_options_defaults();
	mogbus_server_t*	server  = NULL;
	mogbus_client_t*	client  = NULL;
	pthread_t		thread;
	bool			running = false;
	int			status_code = 1;

	if (!dev ||
	    mogbus_device_add_point(dev, &point) != MOGBUS_OK ||
	    !mogbus_device_add_function_code(dev, STATUS_CODE, status, NULL))
	{
		fprintf(stderr, "device: %s\n", ctx ? mogbus_last_error(ctx) : "out of memory");
		goto done;
	}

	so.port = co.port = free_port();
	server = mogbus_server_create(ctx, &so);
	if (!server)
	{
		fprintf(stderr, "server: %s\n", mogbus_last_error(ctx));
		goto done;
	}
	running = pthread_create(&thread, NULL, serve, server) == 0;

	/* The client knows nothing about the server object: host, port, unit and function code. */
	client = mogbus_client_open(&co);
	if (!running || !client)
	{
		fprintf(stderr, "client: %s\n", client ? "no server thread" : mogbus_last_error(NULL));
		goto done;
	}
	{
		static const uint8_t	ask[] = { 0x01 };
		static const uint8_t	bad[] = { 0x09 };
		uint8_t			reply[252];
		size_t			n = 0;

		if (mogbus_client_request(client, 1, STATUS_CODE, ask, sizeof(ask), reply, sizeof(reply), &n) != MOGBUS_OK ||
		    n != 2 ||
		    ((reply[0] << 8) | reply[1]) != 1234)
		{
			fprintf(stderr, "status: %s\n", mogbus_client_last_error(client));
			goto done;
		}
		printf("function code 0x%02X -> %d\n", STATUS_CODE, (reply[0] << 8) | reply[1]);

		if (mogbus_client_request(client, 1, STATUS_CODE, bad, sizeof(bad), reply, sizeof(reply), &n) != MOGBUS_ERR_EXCEPTION ||
		    mogbus_client_last_exception(client) != 3)
		{
			fprintf(stderr, "expected exception 3\n");
			goto done;
		}
		printf("bad request -> exception %u\n", mogbus_client_last_exception(client));
	}
	status_code = 0;

done:
	mogbus_client_close(client);
	if (running)
	{
		mogbus_server_stop(server);
		pthread_join(thread, NULL);
	}
	mogbus_server_close(server);
	mogbus_destroy(ctx);
	return status_code;
}
