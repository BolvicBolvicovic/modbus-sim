#define _GNU_SOURCE
#include "vtest.h"
#include "modbus/bytes.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "util/clock.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static bool
send_all_fd(int fd, const u8* buf, u32 len)
{
	while (len > 0)
	{
		ssize_t sent = send(fd, buf, len, MSG_NOSIGNAL);
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
recv_exact_fd(int fd, u8* buf, u32 len, u32 timeout_ms)
{
	struct timeval tv = { .tv_sec = (time_t)(timeout_ms / 1000),
			      .tv_usec = (suseconds_t)(timeout_ms % 1000) * 1000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	while (len > 0)
	{
		ssize_t got = recv(fd, buf, len, 0);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return false;
		buf += got;
		len -= (u32)got;
	}
	return true;
}

static bool
recv_rtu_resp(int fd, u8* addr, u8* pdu, u32* pdu_len, u32 timeout_ms)
{
	u8 frame[MODBUS_RTU_MAX_FRAME];
	u32 got = 0;

	while (got < MODBUS_RTU_MAX_FRAME)
	{
		const u8* p = NULL;
		u32 l = 0;
		if (!recv_exact_fd(fd, frame + got, 1, timeout_ms))
			return false;
		got++;
		if (got >= 4 && modbus_rtu_decode(frame, got, addr, &p, &l))
		{
			memcpy(pdu, p, l);
			*pdu_len = l;
			return true;
		}
	}
	return false;
}

static pid_t
start_rtu_tcp(const char* cfg, u16 port)
{
	const char* bin = getenv("MODBUS_SIM_BIN");
	char portbuf[16];
	char* argv[] = { (char*)(bin ? bin : "./modbus-sim"), "--rtu-tcp", "--port", portbuf,
			 "--bind", "127.0.0.1", (char*)cfg, NULL };
	pid_t pid;

	snprintf(portbuf, sizeof(portbuf), "%u", port);
	pid = fork();
	if (pid == 0)
	{
		execv(argv[0], argv);
		_exit(127);
	}
	return pid;
}

static int
connect_port(u16 port, u32 tries)
{
	for (u32 i = 0; i < tries; i++)
	{
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = { 0 };
		a.sin_family = AF_INET;
		a.sin_port = htons(port);
		inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
		if (connect(fd, (struct sockaddr*)&a, sizeof(a)) == 0)
			return fd;
		close(fd);
		usleep(20000);
	}
	return -1;
}

static u16
find_free_port(void)
{
	struct sockaddr_in addr = { 0 };
	socklen_t len = sizeof(addr);
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	u16 port = 0;
	if (fd < 0)
		return 0;
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0 &&
	    getsockname(fd, (struct sockaddr*)&addr, &len) == 0)
		port = ntohs(addr.sin_port);
	close(fd);
	return port;
}

static void
write_cfg(char* cfg_path, size_t cfg_size, const char* body)
{
	int cfd;
	FILE* f;
	snprintf(cfg_path, cfg_size, "/tmp/modbus-sim-rtutcp-XXXXXX");
	cfd = mkstemp(cfg_path);
	ASSERT(cfd >= 0);
	f = fdopen(cfd, "w");
	ASSERT(f != NULL);
	fputs(body, f);
	fclose(f);
}

TEST(RtuOverTcpServesReadHoldingRegisters)
{
	char cfg[64];
	pid_t pid;
	int fd;
	u8 req_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME];
	u8 resp[MODBUS_MAX_PDU];
	u8 addr = 0;
	u32 plen = 0, flen;
	u16 port = find_free_port();

	ASSERT(port != 0);
	write_cfg(cfg, sizeof(cfg), "(server (port 1502))\n(device (unit-id 1) (holding 0 (static 4660)))\n");

	pid = start_rtu_tcp(cfg, port);
	ASSERT(pid > 0);
	fd = connect_port(port, 100);
	ASSERT(fd >= 0);

	flen = modbus_rtu_encode(frame, 1, req_pdu, sizeof(req_pdu));
	ASSERT(flen > 0);
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(plen == 4);
	ASSERT_EQ(get_be16(resp + 2) == 4660);

	close(fd);
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	unlink(cfg);
}

TEST(RtuOverTcpWriteThenRead)
{
	char cfg[64];
	pid_t pid;
	int fd;
	u8 write_pdu[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x01, 0x00, 0x2A };
	u8 read_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x01, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;
	u16 port = find_free_port();

	ASSERT(port != 0);
	write_cfg(cfg, sizeof(cfg),
		  "(device (unit-id 7)\n"
		  "  (holding 0 (static 0))\n"
		  "  (holding 1 (static 0)))\n");
	pid = start_rtu_tcp(cfg, port);
	ASSERT(pid > 0);
	fd = connect_port(port, 100);
	ASSERT(fd >= 0);

	flen = modbus_rtu_encode(frame, 7, write_pdu, sizeof(write_pdu));
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(addr == 7);
	ASSERT_EQ(resp[0] == FC_WRITE_SINGLE_REGISTER);

	flen = modbus_rtu_encode(frame, 7, read_pdu, sizeof(read_pdu));
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(get_be16(resp + 2) == 0x002A);

	close(fd);
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	unlink(cfg);
}

TEST(RtuOverTcpBadCrcGetsNoResponse)
{
	char cfg[64];
	pid_t pid;
	int fd;
	u8 req_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;
	u16 port = find_free_port();

	ASSERT(port != 0);
	write_cfg(cfg, sizeof(cfg), "(device (unit-id 1) (holding 0 (static 1)))\n");
	pid = start_rtu_tcp(cfg, port);
	ASSERT(pid > 0);
	fd = connect_port(port, 100);
	ASSERT(fd >= 0);

	flen = modbus_rtu_encode(frame, 1, req_pdu, sizeof(req_pdu));
	frame[flen - 1] ^= 0xFF; /* corrupt CRC */
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 250) == false);

	close(fd);
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	unlink(cfg);
}

TEST(RtuOverTcpUnknownUnitGetsNoResponse)
{
	char cfg[64];
	pid_t pid;
	int fd;
	u8 req_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;
	u16 port = find_free_port();

	ASSERT(port != 0);
	write_cfg(cfg, sizeof(cfg), "(device (unit-id 1) (holding 0 (static 1)))\n");
	pid = start_rtu_tcp(cfg, port);
	ASSERT(pid > 0);
	fd = connect_port(port, 100);
	ASSERT(fd >= 0);

	flen = modbus_rtu_encode(frame, 99, req_pdu, sizeof(req_pdu));
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 250) == false);

	close(fd);
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	unlink(cfg);
}

TEST(RtuOverTcpBroadcastGetsNoResponse)
{
	char cfg[64];
	pid_t pid;
	int fd;
	u8 req_pdu[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;
	u16 port = find_free_port();

	ASSERT(port != 0);
	write_cfg(cfg, sizeof(cfg), "(device (unit-id 1) (holding 0 (static 0)))\n");
	pid = start_rtu_tcp(cfg, port);
	ASSERT(pid > 0);
	fd = connect_port(port, 100);
	ASSERT(fd >= 0);

	flen = modbus_rtu_encode(frame, 0, req_pdu, sizeof(req_pdu));
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 250) == false);

	close(fd);
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	unlink(cfg);
}

static bool
wait_for_eof_fd(int fd, u32 timeout_ms)
{
	u8 b;
	struct timeval tv = { .tv_sec = (time_t)(timeout_ms / 1000),
			      .tv_usec = (suseconds_t)(timeout_ms % 1000) * 1000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	for (;;)
	{
		ssize_t got = recv(fd, &b, 1, 0);
		if (got == 0)
			return true;
		if (got < 0 && errno == EINTR)
			continue;
		return false;
	}
}

TEST(RtuOverTcpHalfCloseAfterRequestSendsResponse)
{
	char cfg[64];
	pid_t pid;
	int fd;
	u8 req_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;
	u16 port = find_free_port();

	ASSERT(port != 0);
	write_cfg(cfg, sizeof(cfg), "(device (unit-id 1) (holding 0 (static 4660)))\n");
	pid = start_rtu_tcp(cfg, port);
	ASSERT(pid > 0);
	fd = connect_port(port, 100);
	ASSERT(fd >= 0);

	flen = modbus_rtu_encode(frame, 1, req_pdu, sizeof(req_pdu));
	ASSERT(flen > 0);
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(shutdown(fd, SHUT_WR) == 0);
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(plen == 4);
	ASSERT_EQ(get_be16(resp + 2) == 4660);
	ASSERT_EQ(wait_for_eof_fd(fd, 2000));

	close(fd);
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	unlink(cfg);
}

TEST(RtuOverTcpHalfCloseAfterDelayedRequestSendsResponse)
{
	char cfg[64];
	pid_t pid;
	int fd;
	u8 req_pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME], resp[MODBUS_MAX_PDU], addr = 0;
	u32 plen = 0, flen;
	u16 port = find_free_port();
	u64 start;

	ASSERT(port != 0);
	write_cfg(cfg, sizeof(cfg),
		  "(device (unit-id 1)\n"
		  "  (holding 0 (static 4660))\n"
		  "  (fault (kind delay) (delay-ms 200) (percent 100)))\n");
	pid = start_rtu_tcp(cfg, port);
	ASSERT(pid > 0);
	fd = connect_port(port, 100);
	ASSERT(fd >= 0);

	flen = modbus_rtu_encode(frame, 1, req_pdu, sizeof(req_pdu));
	ASSERT(flen > 0);
	start = (u64)clock_now_ms();
	ASSERT_EQ(send_all_fd(fd, frame, flen));
	ASSERT_EQ(shutdown(fd, SHUT_WR) == 0);
	ASSERT_EQ(recv_rtu_resp(fd, &addr, resp, &plen, 2000));
	ASSERT_EQ(clock_now_ms() - start >= 180);
	ASSERT_EQ(addr == 1);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS);
	ASSERT_EQ(plen == 4);
	ASSERT_EQ(get_be16(resp + 2) == 4660);
	ASSERT_EQ(wait_for_eof_fd(fd, 2000));

	close(fd);
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	unlink(cfg);
}
