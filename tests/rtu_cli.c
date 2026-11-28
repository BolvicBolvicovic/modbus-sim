#define _GNU_SOURCE
#include "vtest.h"
#include "mogbus.h"
#include "modbus/bytes.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "os/os_time.h"
#include "tests/tests_utils/test_support.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

typedef ts_server_t rtu_cli_t;

static ts_server_t g_rtu_cli = {
	.pid		= -1,
	.master_fd	= -1,
	.flags		= TS_CAPTURE_STDERR | TS_NULL_STDIN | TS_NULL_STDOUT | TS_EXIT_IS_READY,
	.label		= "rtu cli",
	.ready_needle	= "RTU listening on"
};

static bool
rtu_cli_verbose(void)
{
	return ts_verbose();
}

static void
rtu_cli_dump_stderr(void)
{
	ts_dump_stderr(&g_rtu_cli);
}

static void
rtu_cli_stop(void)
{
	g_rtu_cli.flags &= ~(u32)TS_CAPTURE_STDOUT;
	ASSERT_EQ(ts_server_stop(&g_rtu_cli));
}

static bool
rtu_cli_write_config(const char* text)
{
	return ts_write_config(&g_rtu_cli, "/tmp/mogbus-rtucli-XXXXXX", text);
}

static bool
rtu_cli_capture_stderr(void)
{
	return ts_temp_file(g_rtu_cli.stderr_path, sizeof(g_rtu_cli.stderr_path),
			    "/tmp/mogbus-rtuclierr-XXXXXX");
}

static bool
rtu_cli_file_contains(const char* path, const char* needle)
{
	return ts_file_contains(path, needle);
}

static bool
rtu_cli_stderr_contains(const char* needle)
{
	return ts_file_contains(g_rtu_cli.stderr_path, needle);
}

static bool
rtu_cli_wait_startup(pid_t pid)
{
	return ts_wait_startup(&g_rtu_cli, pid);
}

static bool
rtu_cli_start_with_args(char* const argv[], bool capture_stderr)
{
	if (capture_stderr)
		g_rtu_cli.flags |= TS_CAPTURE_STDERR;
	else
		g_rtu_cli.flags &= ~(u32)TS_CAPTURE_STDERR;
	if (capture_stderr && !ts_verbose() && !g_rtu_cli.stderr_path[0] &&
	    !rtu_cli_capture_stderr())
		return false;
	return ts_server_start(&g_rtu_cli, argv);
}

static bool
rtu_cli_start_with_args_capture_stdout(char* const argv[])
{
	bool ok;

	if (!ts_verbose() && !g_rtu_cli.stderr_path[0] && !rtu_cli_capture_stderr())
		return false;
	if (!ts_temp_file(g_rtu_cli.stdout_path, sizeof(g_rtu_cli.stdout_path),
			  "/tmp/mogbus-rtucliout-XXXXXX"))
		return false;
	g_rtu_cli.flags |= TS_CAPTURE_STDOUT;
	ok = ts_server_start(&g_rtu_cli, argv);
	g_rtu_cli.flags &= ~(u32)TS_CAPTURE_STDOUT;
	return ok;
}

static bool
send_all_fd(int fd, const u8* buf, u32 len)
{
	return ts_send_fd(fd, buf, len);
}

static bool
recv_rtu_resp_fd(int fd, u8* addr, u8* pdu, u32* pdu_len, u32 timeout_ms)
{
	return ts_rtu_recv(fd, addr, pdu, pdu_len, timeout_ms, false);
}

static bool
recv_none_fd(int fd, u32 timeout_ms)
{
	return ts_no_data(fd, timeout_ms);
}

static bool
send_rtu_req_fd(int fd, u8 addr, const u8* pdu, u32 pdu_len)
{
	return ts_rtu_send(fd, addr, pdu, pdu_len, false);
}

static bool
wait_for_process_exit(pid_t pid, int* status, u32 timeout_ms)
{
	return ts_wait_pid(pid, status, timeout_ms);
}

static bool
rtu_cli_write_server_config(const char* mode, const char* path, const char* options,
			    const char* devices)
{
	char text[1024];

	if (path)
		snprintf(text, sizeof(text), "(server (mode %s) (path \"%s\") %s)\n%s\n",
			 mode, path, options ? options : "", devices);
	else
		snprintf(text, sizeof(text), "(server (mode %s) %s)\n%s\n",
			 mode, options ? options : "", devices);
	return rtu_cli_write_config(text);
}

static bool
rtu_cli_start_pty(const char* options, const char* devices)
{
	const char* bin = getenv("MOGBUS_BIN");
	char* argv[3];
	int   slave = -1;
	char  name[128];

	if (!bin)
		bin = "./mogbus";
	if (openpty(&g_rtu_cli.master_fd, &slave, name, NULL, NULL) < 0)
		return false;
	{
		struct termios tio;
		if (tcgetattr(g_rtu_cli.master_fd, &tio) == 0)
		{
			cfmakeraw(&tio);
			(void)tcsetattr(g_rtu_cli.master_fd, TCSANOW, &tio);
		}
	}
	close(slave);
	if (!rtu_cli_write_server_config("rtu-device", name, options, devices))
		return false;
	argv[0] = (char*)bin;
	argv[1] = g_rtu_cli.config;
	argv[2] = NULL;
	return rtu_cli_start_with_args(argv, true);
}

TEST(RtuCliServesRtuOverPtyDevice)
{
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 resp[MODBUS_MAX_PDU];
	u8 addr = 0;
	u32 len = 0;
	bool ok = false;

	ASSERT_EQ(rtu_cli_start_pty("", "(device (unit-id 1) (holding 0 (static 4660)))\n"));

	ok = send_rtu_req_fd(g_rtu_cli.master_fd, 1, req, sizeof(req));
	ok = ok && recv_rtu_resp_fd(g_rtu_cli.master_fd, &addr, resp, &len, 1000);
	ok = ok && addr == 1 && len == 4 && resp[0] == FC_READ_HOLDING_REGISTERS &&
	     get_be16(resp + 2) == 0x1234;

	rtu_cli_stop();
	ASSERT_EQ(ok);
}

TEST(RtuCliConfigFrameGapAffectsResponseTiming)
{
	/* Use a large configured frame gap so a fragmented frame is not processed
	 * until the gap elapses. Sending the first half, waiting 50 ms (< gap),
	 * then sending the rest must not respond until the gap has passed. */
	u8 pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME];
	u32 flen;
	u8 resp[MODBUS_MAX_PDU];
	u8 addr = 0;
	u32 len = 0;
	bool ok = false;
	u64 start;

	ASSERT_EQ(rtu_cli_start_pty("(frame-gap-ms 300)",
				    "(device (unit-id 1) (holding 0 (static 7)))\n"));
	flen = modbus_rtu_encode(frame, 1, pdu, sizeof(pdu));
	ASSERT_EQ(flen > 4);

	ok = send_all_fd(g_rtu_cli.master_fd, frame, flen / 2);
	if (ok)
	{
		u8 tmp_addr = 0;
		u8 tmp_pdu[MODBUS_MAX_PDU];
		u32 tmp_len = 0;
		ok = !recv_rtu_resp_fd(g_rtu_cli.master_fd, &tmp_addr, tmp_pdu, &tmp_len, 80);
	}
	if (ok)
	{
		start = os_now_ms();
		ok = send_all_fd(g_rtu_cli.master_fd, frame + flen / 2, flen - (flen / 2));
		ok = ok && recv_rtu_resp_fd(g_rtu_cli.master_fd, &addr, resp, &len, 1000);
		ok = ok && (os_now_ms() - start) >= 200;
		ok = ok && addr == 1 && get_be16(resp + 2) == 7;
	}

	rtu_cli_stop();
	ASSERT_EQ(ok);
}

TEST(RtuCliCheckSkipsReservedUnitIdWarningWithoutSerialMode)
{
	const char* bin = getenv("MOGBUS_BIN");
	char* argv[] = { (char*)(bin ? bin : "./mogbus"), "--check", g_rtu_cli.config, NULL };
	int status = 0;
	bool ok;

	ASSERT_EQ(rtu_cli_write_config("(device (unit-id 0) (holding 0))\n"));
	ok = rtu_cli_start_with_args(argv, true);
	ASSERT_EQ(ok);
	ASSERT_EQ(wait_for_process_exit(g_rtu_cli.pid, &status, 2000));
	ASSERT_EQ(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	ASSERT_EQ(!rtu_cli_file_contains(g_rtu_cli.stderr_path, "not valid for RTU"));
	ASSERT_EQ(!rtu_cli_file_contains(g_rtu_cli.stderr_path, "warning:"));
	g_rtu_cli.pid = -1;
	rtu_cli_stop();
}

TEST(CliVersionPrintsLibraryVersion)
{
	const char* bin = getenv("MOGBUS_BIN");
	char cmd[256], line[128] = "", want[64];
	FILE* f;

	snprintf(cmd, sizeof(cmd), "%s --version", bin ? bin : "./mogbus");
	snprintf(want, sizeof(want), "mogbus %s", mogbus_version());
	f = popen(cmd, "r");
	ASSERT_EQ(f != NULL);
	ASSERT_EQ(fgets(line, sizeof(line), f) != NULL);
	ASSERT_EQ(pclose(f) == 0);
	ASSERT_EQ(strncmp(line, want, strlen(want)) == 0);
}

TEST(CliLogFormatRejectsUnknownValues)
{
	const char* bin = getenv("MOGBUS_BIN");
	char cmd[256];
	int status;

	snprintf(cmd, sizeof(cmd), "%s --log-format yaml profiles/minimal.scm 2>/dev/null", bin ? bin : "./mogbus");
	status = system(cmd);
	ASSERT_EQ(WIFEXITED(status) && WEXITSTATUS(status) == 2);
	snprintf(cmd, sizeof(cmd), "%s --log-format json --check profiles/minimal.scm >/dev/null", bin ? bin : "./mogbus");
	status = system(cmd);
	ASSERT_EQ(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static bool
rtu_cli_start_real_pty(const char* options)
{
	rtu_cli_t* fx = &g_rtu_cli;
	char* argv[8];
	int argc = 0;
	const char* bin = getenv("MOGBUS_BIN");

	if (!bin)
		bin = "./mogbus";
	if (!rtu_cli_write_server_config("rtu-pty", NULL, options,
					 "(device (unit-id 1) (holding 0 (static 4660)))\n"))
		return false;
	argv[argc++] = (char*)bin;
	argv[argc++] = fx->config;
	argv[argc] = NULL;
	return rtu_cli_start_with_args_capture_stdout(argv);
}

/* Opens the slave path the rtu-pty server printed on stdout; -1 when it never did. */
static int
rtu_cli_open_printed_slave(void)
{
	FILE* out = fopen(g_rtu_cli.stdout_path, "r");
	char line[512];
	char slave_path[128] = "";

	for (u32 tries = 0; out && tries < 40 && !slave_path[0]; tries++)
	{
		rewind(out);
		while (fgets(line, sizeof(line), out))
		{
			char* p = strstr(line, "RTU PTY slave: ");

			if (!p)
				continue;
			p += strlen("RTU PTY slave: ");
			snprintf(slave_path, sizeof(slave_path), "%.*s", (int)strcspn(p, "\r\n"), p);
			break;
		}
		if (!slave_path[0])
			usleep(50000);
	}
	if (out)
		fclose(out);
	return slave_path[0] ? open(slave_path, O_RDWR | O_NOCTTY | O_CLOEXEC) : -1;
}

TEST(RtuCliPtyModePrintsSlavePathAndServesRequests)
{
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 resp[MODBUS_MAX_PDU];
	u8 addr = 0;
	u32 len = 0;
	bool ok;
	struct termios tio;

	ASSERT_EQ(rtu_cli_start_real_pty("(frame-gap-ms 5)"));
	g_rtu_cli.master_fd = rtu_cli_open_printed_slave();
	ASSERT(g_rtu_cli.master_fd >= 0);
	if (tcgetattr(g_rtu_cli.master_fd, &tio) == 0)
	{
		cfmakeraw(&tio);
		(void)tcsetattr(g_rtu_cli.master_fd, TCSANOW, &tio);
	}
	ok = send_rtu_req_fd(g_rtu_cli.master_fd, 1, req, sizeof(req));
	ok = ok && recv_rtu_resp_fd(g_rtu_cli.master_fd, &addr, resp, &len, 1500);
	ok = ok && addr == 1 && len == 4 && get_be16(resp + 2) == 0x1234;
	rtu_cli_stop();
	ASSERT_EQ(ok);
}


TEST(RtuCliDeviceOpenDoesNotBlockOnFifoWithoutWriter)
{
	const char* bin = getenv("MOGBUS_BIN");
	char fifo_path[] = "/tmp/mogbus-rtu-fifo-XXXXXX";
	char* argv[8];
	int argc = 0;
	int fd;
	pid_t pid;
	int status = 0;
	bool ok;

	if (!bin)
		bin = "./mogbus";
	fd = mkstemp(fifo_path);
	ASSERT_EQ(fd >= 0);
	close(fd);
	unlink(fifo_path);
	ASSERT_EQ(mkfifo(fifo_path, 0600) == 0);
	ASSERT_EQ(rtu_cli_write_server_config("rtu-device", fifo_path, "",
					      "(device (unit-id 1) (holding 0 (static 1)))\n"));
	if (!rtu_cli_verbose())
		ASSERT_EQ(rtu_cli_capture_stderr());

	argv[argc++] = (char*)bin;
	argv[argc++] = g_rtu_cli.config;
	argv[argc] = NULL;

	pid = fork();
	if (pid == 0)
	{
		if (!freopen("/dev/null", "r", stdin))
			_exit(127);
		if (!freopen("/dev/null", "w", stdout))
			_exit(127);
		if (g_rtu_cli.stderr_path[0] && !rtu_cli_verbose())
		{
			if (!freopen(g_rtu_cli.stderr_path, "w", stderr))
				_exit(127);
		}
		else if (!freopen("/dev/null", "w", stderr))
			_exit(127);
		execv(argv[0], argv);
		_exit(127);
	}
	ASSERT_EQ(pid > 0);

	/* A blocking open() would never reach the "listening" log line, which is
	 * emitted just before the RTU poll loop. A FIFO opened O_RDWR has no EOF
	 * on Linux, so terminate the process after confirming it started. */
	ok = rtu_cli_wait_startup(pid);
	if (ok && !rtu_cli_verbose())
		ok = rtu_cli_stderr_contains("RTU listening on");
	kill(pid, SIGTERM);
	ok = ok && wait_for_process_exit(pid, &status, 3000);
	if (!ok && waitpid(pid, &status, WNOHANG) != pid)
	{
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
	}

	unlink(fifo_path);
	rtu_cli_stop();
	ASSERT_EQ(ok);
}

static bool
rtu_termios_is_8n1_raw(const struct termios* tio)
{
	if ((tio->c_cflag & CSIZE) != CS8)
		return false;
	if ((tio->c_cflag & PARENB) != 0)
		return false;
	if ((tio->c_cflag & CSTOPB) != 0)
		return false;
	if ((tio->c_cflag & (CLOCAL | CREAD)) != (CLOCAL | CREAD))
		return false;
	if ((tio->c_cflag & CRTSCTS) != 0)
		return false;
	if ((tio->c_iflag & (IXON | IXOFF | IXANY)) != 0)
		return false;
	if ((tio->c_lflag & (ICANON | ECHO | ECHOE | ISIG | IEXTEN)) != 0)
		return false;
	if ((tio->c_oflag & OPOST) != 0)
		return false;
	return true;
}

TEST(RtuCliRtuDeviceSets8N1RawTermiosAndPreservesBaud)
{
	const char* bin = getenv("MOGBUS_BIN");
	int master_fd = -1;
	int slave_fd = -1;
	char name[128];
	struct termios tio;
	struct termios after;
	pid_t pid;
	int status = 0;
	bool ok = false;

	if (!bin)
		bin = "./mogbus";
	ASSERT_EQ(openpty(&master_fd, &slave_fd, name, NULL, NULL) == 0);

	/* Preconfigure a weird/non-8N1 state with a known baud rate. The server
	 * must leave the tty in 8N1 raw mode while preserving baud. */
	ASSERT_EQ(tcgetattr(slave_fd, &tio) == 0);
	cfmakeraw(&tio);
	cfsetispeed(&tio, B19200);
	cfsetospeed(&tio, B19200);
	tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CLOCAL | CREAD | CRTSCTS);
	tio.c_cflag |= CS7 | PARENB | CSTOPB | CRTSCTS;
	tio.c_iflag |= IXON | IXOFF | IXANY;
	ASSERT_EQ(tcsetattr(slave_fd, TCSANOW, &tio) == 0);
	ASSERT_EQ(rtu_cli_write_server_config("rtu-device", name, "",
					      "(device (unit-id 1) (holding 0 (static 1)))\n"));
	if (!rtu_cli_verbose())
		ASSERT_EQ(rtu_cli_capture_stderr());

	pid = fork();
	if (pid == 0)
	{
		char* argv[] = { (char*)bin, g_rtu_cli.config, NULL };
		close(master_fd);
		close(slave_fd);
		if (!freopen("/dev/null", "r", stdin))
			_exit(127);
		if (!freopen("/dev/null", "w", stdout))
			_exit(127);
		if (g_rtu_cli.stderr_path[0] && !rtu_cli_verbose())
		{
			if (!freopen(g_rtu_cli.stderr_path, "w", stderr))
				_exit(127);
		}
		else if (!freopen("/dev/null", "w", stderr))
			_exit(127);
		execv(argv[0], argv);
		_exit(127);
	}
	ASSERT_EQ(pid > 0);

	/* The startup log line is emitted after the device is opened and configured. */
	ok = rtu_cli_wait_startup(pid);
	if (ok && !rtu_cli_verbose())
		ok = rtu_cli_stderr_contains("RTU listening on");
	if (ok)
	{
		ASSERT_EQ(tcgetattr(slave_fd, &after) == 0);
		ok = rtu_termios_is_8n1_raw(&after);
		ok = ok && cfgetispeed(&after) == B19200;
		ok = ok && cfgetospeed(&after) == B19200;
	}

	kill(pid, SIGTERM);
	wait_for_process_exit(pid, &status, 3000);
	if (waitpid(pid, &status, WNOHANG) != pid)
	{
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
	}
	if (master_fd >= 0)
		close(master_fd);
	if (slave_fd >= 0)
		close(slave_fd);
	rtu_cli_stop();
	ASSERT_EQ(ok);
}

static bool
rtu_cli_line_flags_match(const struct termios* tio, tcflag_t set)
{
	/* Linux PTYs strip PARENB from tcsetattr(); PARODD, CMSPAR and CSTOPB are
	 * observable. tests/serial.c checks the requested PARENB bit directly. */
	tcflag_t flags = PARODD | CSTOPB;

#ifdef CMSPAR
	flags |= CMSPAR;
#endif
	return (tio->c_cflag & CSIZE) == CS8 &&
	       (tio->c_cflag & flags) == (set & flags) &&
	       (tio->c_cflag & (CLOCAL | CREAD)) == (CLOCAL | CREAD) &&
	       (tio->c_cflag & CRTSCTS) == 0;
}

/* Starts the server on a PTY slave preconfigured with non-default line flags,
 * then returns the slave's termios so tests can inspect the applied flags. */
static bool
rtu_cli_pty_line_flags(const char* options, const char* devices, struct termios* after)
{
	rtu_cli_t* fx = &g_rtu_cli;
	char* argv[8];
	int argc = 0;
	const char* bin = getenv("MOGBUS_BIN");
	int master_fd = -1;
	int slave_fd = -1;
	char name[128];
	struct termios tio;
	pid_t pid;
	int status = 0;
	bool ok = false;

	if (!bin)
		bin = "./mogbus";
	if (openpty(&master_fd, &slave_fd, name, NULL, NULL) < 0)
		goto done;
	if (tcgetattr(slave_fd, &tio) != 0)
		goto done;
	cfmakeraw(&tio);
	cfsetispeed(&tio, B19200);
	cfsetospeed(&tio, B19200);
	tio.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
#ifdef CMSPAR
	tio.c_cflag &= ~CMSPAR;
#endif
	tio.c_cflag |= CS7 | PARENB | PARODD | CSTOPB | CRTSCTS;
	if (tcsetattr(slave_fd, TCSANOW, &tio) != 0)
		goto done;
	if (!rtu_cli_write_server_config("rtu-device", name, options, devices))
		goto done;
	if (!rtu_cli_verbose() && !rtu_cli_capture_stderr())
		goto done;

	argv[argc++] = (char*)bin;
	argv[argc++] = fx->config;
	argv[argc] = NULL;

	pid = fork();
	if (pid < 0)
		goto done;
	if (pid == 0)
	{
		close(master_fd);
		close(slave_fd);
		if (!freopen("/dev/null", "r", stdin))
			_exit(127);
		if (!freopen("/dev/null", "w", stdout))
			_exit(127);
		if (fx->stderr_path[0] && !rtu_cli_verbose())
		{
			if (!freopen(fx->stderr_path, "w", stderr))
				_exit(127);
			setvbuf(stderr, NULL, _IONBF, 0);
		}
		else if (!freopen("/dev/null", "w", stderr))
			_exit(127);
		execv(argv[0], argv);
		_exit(127);
	}
	fx->pid = pid;
	ok = rtu_cli_wait_startup(pid);
	if (ok && !rtu_cli_verbose())
		ok = rtu_cli_stderr_contains("RTU listening on");
	if (ok)
		ok = tcgetattr(slave_fd, after) == 0;
	ok = ok && cfgetispeed(after) == B19200;
	ok = ok && cfgetospeed(after) == B19200;

	kill(pid, SIGTERM);
	if (!wait_for_process_exit(pid, &status, 3000) &&
	    waitpid(pid, &status, WNOHANG) != pid)
	{
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
	}
	fx->pid = -1;

done:
	if (master_fd >= 0)
		close(master_fd);
	if (slave_fd >= 0)
		close(slave_fd);
	rtu_cli_stop(); /* cleans config/stderr paths; the child was already reaped */
	return ok;
}

TEST(RtuCliLineFlagsFromConfig)
{
	static const struct { const char* options; tcflag_t flags; } cases[] = {
		{ "", 0 },
		{ "(parity even)", PARENB },
		{ "(parity odd)", PARENB | PARODD },
#ifdef CMSPAR
		{ "(parity mark)", PARENB | PARODD | CMSPAR },
		{ "(parity space)", PARENB | CMSPAR },
#endif
		{ "(stop-bits 2)", CSTOPB },
		{ "(stop-bits 1)", 0 },
	};

	for (u32 c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
	{
		struct termios after;
		bool ok = rtu_cli_pty_line_flags(cases[c].options, "(device (unit-id 1) (holding 0))\n", &after) &&
			  rtu_cli_line_flags_match(&after, cases[c].flags);

		if (!ok)
			fprintf(stderr, "line flags case failed: \"%s\"\n", cases[c].options);
		ASSERT_EQ(ok);
	}
}

TEST(RtuCliConfigBaudAffectsRtuTiming)
{
	/* 9600 baud -> t3.5 ~4.01ms, effective 5ms. Configured baud must make the
	 * server treat byte gaps larger than t3.5 as frame boundaries. */
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME];
	u8 resp[MODBUS_MAX_PDU];
	u8 addr = 0;
	u32 len = 0;
	u32 flen = 0;
	bool ok = true;

	ASSERT_EQ(rtu_cli_start_pty("(baud 9600)",
				    "(device (unit-id 1) (holding 0 (static 4660)))\n"));
	flen = modbus_rtu_encode(frame, 1, req, sizeof(req));

	for (u32 i = 0; i < flen && ok; i++)
	{
		ok = send_all_fd(g_rtu_cli.master_fd, frame + i, 1);
		if (ok && i + 1 < flen)
			usleep(1000); /* < t3.5 */
	}
	if (ok) ok = recv_rtu_resp_fd(g_rtu_cli.master_fd, &addr, resp, &len, 1000);
	if (ok) ok = addr == 1 && get_be16(resp + 2) == 4660;

	if (ok) ok = send_all_fd(g_rtu_cli.master_fd, frame, flen / 2);
	if (ok) usleep(20000); /* > t3.5 */
	if (ok) ok = send_all_fd(g_rtu_cli.master_fd, frame + flen / 2, flen - flen / 2);
	if (ok)
	{
		u8 tmp_addr = 0, tmp_pdu[MODBUS_MAX_PDU];
		u32 tmp_len = 0;
		ok = !recv_rtu_resp_fd(g_rtu_cli.master_fd, &tmp_addr, tmp_pdu, &tmp_len, 120);
	}
	if (ok) ok = send_rtu_req_fd(g_rtu_cli.master_fd, 1, req, sizeof(req));
	if (ok) ok = recv_rtu_resp_fd(g_rtu_cli.master_fd, &addr, resp, &len, 800);
	if (ok) ok = addr == 1 && get_be16(resp + 2) == 4660;

	rtu_cli_stop();
	ASSERT_EQ(ok);
}

TEST(RtuCliConfigFrameGapBeatsBaud)
{
	/* Baud-derived t3.5 at 9600 is ~4ms, but an explicit frame-gap must win. */
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME];
	u8 resp[MODBUS_MAX_PDU];
	u8 addr = 0;
	u32 len = 0;
	u32 flen = 0;
	bool ok = false;

	ASSERT_EQ(rtu_cli_start_pty("(baud 9600) (frame-gap-ms 80)",
				    "(device (unit-id 1) (holding 0 (static 4660)))\n"));
	flen = modbus_rtu_encode(frame, 1, req, sizeof(req));
	ASSERT(flen > 4);

	ok = send_all_fd(g_rtu_cli.master_fd, frame, flen / 2);
	if (ok)
	{
		u8 tmp_addr = 0, tmp_pdu[MODBUS_MAX_PDU];
		u32 tmp_len = 0;
		ok = !recv_rtu_resp_fd(g_rtu_cli.master_fd, &tmp_addr, tmp_pdu, &tmp_len, 30); /* > t3.5, < override */
	}
	if (ok) usleep(30000);
	if (ok) ok = send_all_fd(g_rtu_cli.master_fd, frame + flen / 2, flen - flen / 2);
	if (ok) ok = recv_rtu_resp_fd(g_rtu_cli.master_fd, &addr, resp, &len, 1200);
	if (ok) ok = addr == 1 && get_be16(resp + 2) == 4660;

	rtu_cli_stop();
	ASSERT_EQ(ok);
}

TEST(RtuCliPtyDefaultUsesUnknownBaudFallback)
{
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME];
	u8 resp[MODBUS_MAX_PDU];
	u8 addr = 0;
	u32 len = 0;
	u32 flen = 0;
	bool ok = false;

	/* Real rtu-pty config path: #42 regression (master fd must not auto-detect 38400). */
	ASSERT_EQ(rtu_cli_start_real_pty(""));
	ASSERT_EQ(rtu_cli_stderr_contains("frame gap 20 ms (default; baud unknown)"));
	g_rtu_cli.master_fd = rtu_cli_open_printed_slave();
	ASSERT(g_rtu_cli.master_fd >= 0);

	flen = modbus_rtu_encode(frame, 1, req, sizeof(req));
	ASSERT(flen > 4);
	ok = send_all_fd(g_rtu_cli.master_fd, frame, flen / 2);
	if (ok)
	{
		u8 tmp_addr = 0, tmp_pdu[MODBUS_MAX_PDU];
		u32 tmp_len = 0;
		/* Would be split at baud-derived 2ms for PTY master default 38400; 20ms fallback keeps it together. */
		ok = !recv_rtu_resp_fd(g_rtu_cli.master_fd, &tmp_addr, tmp_pdu, &tmp_len, 5);
	}
	if (ok) usleep(5000);
	if (ok) ok = send_all_fd(g_rtu_cli.master_fd, frame + flen / 2, flen - flen / 2);
	if (ok) ok = recv_rtu_resp_fd(g_rtu_cli.master_fd, &addr, resp, &len, 1000);
	if (ok) ok = addr == 1 && get_be16(resp + 2) == 4660;

	rtu_cli_stop();
	ASSERT_EQ(ok);
}

TEST(RtuCliConfigBaudOnPtyLogsDerivedTiming)
{
	ASSERT_EQ(rtu_cli_start_real_pty("(baud 9600)"));
	ASSERT_EQ(rtu_cli_stderr_contains("baud 9600, t3.5 4011 us (frame gap 5 ms)"));
	rtu_cli_stop();
}

TEST(RtuCliBroadcastExecuteFromConfigEnablesExecution)
{
	u8		write_req[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0x00, 0x2A };
	u8		read_req[]  = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];
	u8		addr = 0;
	u32		len  = 0;
	bool		ok = false;

	ASSERT_EQ(rtu_cli_start_pty("(broadcast-execute on)",
				    "(device (unit-id 1) (holding 0 (static 0)))\n"
				    "(device (unit-id 2) (holding 0 (static 0)))\n"));

	ok = send_rtu_req_fd(g_rtu_cli.master_fd, 0, write_req, sizeof(write_req));
	ok = ok && recv_none_fd(g_rtu_cli.master_fd, 200);
	for (u8 unit = 1; unit <= 2 && ok; unit++)
	{
		ok = send_rtu_req_fd(g_rtu_cli.master_fd, unit, read_req, sizeof(read_req));
		ok = ok && recv_rtu_resp_fd(g_rtu_cli.master_fd, &addr, resp, &len, 1000);
		ok = ok && addr == unit && get_be16(resp + 2) == 0x002A;
	}

	rtu_cli_stop();
	ASSERT_EQ(ok);
}

TEST(RtuCliCheckTcpRejectsSerialLineConfig)
{
	const char* bin = getenv("MOGBUS_BIN");
	char* argv[] = { (char*)(bin ? bin : "./mogbus"), "--check", g_rtu_cli.config, NULL };
	int status = 0;

	ASSERT_EQ(rtu_cli_write_config("(server (mode tcp) (parity even) (stop-bits 2))\n"
				       "(device (unit-id 1) (holding 0))\n"));
	ASSERT_EQ(rtu_cli_start_with_args(argv, true));
	ASSERT_EQ(wait_for_process_exit(g_rtu_cli.pid, &status, 2000));
	ASSERT_EQ(WIFEXITED(status) && WEXITSTATUS(status) == 1);
	ASSERT_EQ(rtu_cli_stderr_contains("parity is not valid in tcp mode"));
	g_rtu_cli.pid = -1;
	rtu_cli_stop();
}

static bool
rtu_cli_expect_cli_error(char* const argv[], const char* message)
{
	int status = 0;
	bool ok = rtu_cli_start_with_args(argv, true);

	ok = ok && wait_for_process_exit(g_rtu_cli.pid, &status, 2000);
	ok = ok && WIFEXITED(status) && WEXITSTATUS(status) == 2;
	ok = ok && rtu_cli_stderr_contains(message);
	g_rtu_cli.pid = -1;
	rtu_cli_stop();
	return ok;
}

static bool
rtu_cli_expect_cli_success(char* const argv[], const char* const* needles)
{
	int status = 0;
	bool ok = rtu_cli_start_with_args_capture_stdout(argv);

	ok = ok && wait_for_process_exit(g_rtu_cli.pid, &status, 2000);
	ok = ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
	for (u32 i = 0; ok && needles && needles[i]; i++)
		ok = rtu_cli_file_contains(g_rtu_cli.stdout_path, needles[i]);
	g_rtu_cli.pid = -1;
	rtu_cli_stop();
	return ok;
}

TEST(RtuCliCheckSummarizesConfiguredModes)
{
	const char* bin = getenv("MOGBUS_BIN");
	const char devices[] = "(device (unit-id 1) (holding 0 (static 1)))\n";
	static const struct {
		const char* mode;
		const char* target;
		const char* options;
		const char* needles[4];
	} cases[] = {
		{ "rtu-device", "/dev/ttyS0", "", { "RTU /dev/ttyS0" } },
		{ "ascii-device", "/dev/ttyUSB0", "", { "ASCII /dev/ttyUSB0" } },
		{ "rtu-pty", NULL, "", { "RTU pty" } },
		{ "rtu-tcp", NULL, "(baud 9600)", { "RTU-over-TCP", "baud 9600, t3.5 4011 us (frame-gap 5 ms)", "config OK" } },
		{ "rtu-tcp", NULL, "(broadcast-execute on)", { "RTU-over-TCP", "broadcast-execute on" } },
	};

	if (!bin)
		bin = "./mogbus";
	for (u32 c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
	{
		char* argv[] = { (char*)bin, "--check=verbose", g_rtu_cli.config, NULL };
		bool ok = rtu_cli_write_server_config(cases[c].mode, cases[c].target, cases[c].options, devices) &&
			  rtu_cli_expect_cli_success(argv, cases[c].needles);

		if (!ok)
			fprintf(stderr, "check summary case failed: %s %s\n", cases[c].mode, cases[c].options);
		ASSERT_EQ(ok);
	}
}

TEST(RtuCliCheckPrintsNothingUnlessVerbose)
{
	const char* bin = getenv("MOGBUS_BIN");
	char* quiet[]   = { (char*)(bin ? bin : "./mogbus"), "--check", g_rtu_cli.config, NULL };
	char* verbose[] = { (char*)(bin ? bin : "./mogbus"), "--check=verbose", g_rtu_cli.config, NULL };
	char* bad[]     = { (char*)(bin ? bin : "./mogbus"), "--check=loud", g_rtu_cli.config, NULL };
	int status = 0;
	bool ok;

	ASSERT_EQ(rtu_cli_write_config("(device (unit-id 1) (holding 0 (static 1)))\n"));
	ok = rtu_cli_start_with_args_capture_stdout(quiet) &&
	     wait_for_process_exit(g_rtu_cli.pid, &status, 2000) &&
	     WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
	     !rtu_cli_file_contains(g_rtu_cli.stdout_path, "config OK") &&
	     !rtu_cli_file_contains(g_rtu_cli.stdout_path, "device");
	g_rtu_cli.pid = -1;
	rtu_cli_stop();
	ASSERT_EQ(ok);

	ASSERT_EQ(rtu_cli_write_config("(device (unit-id 1) (holding 0 (static 1)))\n"));
	ASSERT_EQ(rtu_cli_expect_cli_success(verbose, (const char* const[]){ "1 device", "config OK", NULL }));
	ASSERT_EQ(rtu_cli_write_config("(device (unit-id 1) (holding 0 (static 1)))\n"));
	ASSERT_EQ(rtu_cli_expect_cli_error(bad, "--check takes no value"));
}

TEST(RtuCliAcceptsOnlyGenericOptionsAndRejectsServerFlags)
{
	const char* bin = getenv("MOGBUS_BIN");
	static const char* const removed_flags[] = {
		"-p", "--port", "-b", "--bind", "--rtu-pty", "--rtu-device",
		"--rtu-tcp", "--ascii-pty", "--ascii-device", "--baud", "--rtu-baud",
		"--frame-gap-ms", "--strict-t15", "--broadcast-execute",
		"--parity", "--stop-bits"
	};
	const char valid[] = "(device (unit-id 1) (holding 0 (static 1)))\n";
	bool ok = true;

	if (!bin)
		bin = "./mogbus";

	for (u32 i = 0; i < sizeof(removed_flags) / sizeof(removed_flags[0]); i++)
	{
		char* argv[] = { (char*)bin, (char*)removed_flags[i], (char*)"x",
				 g_rtu_cli.config, NULL };

		ASSERT_EQ(rtu_cli_write_config(valid));
		ok = ok && rtu_cli_expect_cli_error(argv, "unknown option");
	}

	{
		char* argv[] = { (char*)bin, "--help", NULL };

		ok = ok && rtu_cli_expect_cli_success(argv, (const char* const[]){ "defined in (server ...)", NULL });
	}
	{
		char* argv[] = { (char*)bin, "-h", NULL };

		ok = ok && rtu_cli_expect_cli_success(argv, (const char* const[]){ "Usage: mogbus", NULL });
	}
	{
		char* argv[] = { (char*)bin, "--check=verbose", g_rtu_cli.config, NULL };

		ASSERT_EQ(rtu_cli_write_config(valid));
		ok = ok && rtu_cli_expect_cli_success(argv, (const char* const[]){ "config OK", NULL });
	}
	{
		char* argv[] = { (char*)bin, "--verbose", "-c", g_rtu_cli.config, NULL };

		ASSERT_EQ(rtu_cli_write_config(valid));
		ok = ok && rtu_cli_expect_cli_success(argv, NULL);
	}
	{
		char* argv[] = { (char*)bin, "-v", "--check=verbose", g_rtu_cli.config, NULL };

		ASSERT_EQ(rtu_cli_write_config(valid));
		ok = ok && rtu_cli_expect_cli_success(argv, (const char* const[]){ "config OK", NULL });
	}
	ASSERT_EQ(ok);
}
