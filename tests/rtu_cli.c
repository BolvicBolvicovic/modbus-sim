#define _GNU_SOURCE
#include "vtest.h"
#include "modbus/bytes.h"
#include "modbus/pdu.h"
#include "modbus/rtu.h"
#include "util/clock.h"
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

typedef struct rtu_cli_t rtu_cli_t;
struct rtu_cli_t
{
	pid_t	pid;
	int	master_fd;
	char	config[80];
	char	stderr_path[80];
	char	stdout_path[80];
};

static rtu_cli_t g_rtu_cli = { .pid = -1, .master_fd = -1 };

static bool
rtu_cli_verbose(void)
{
	const char* v = getenv("MODBUS_SIM_TEST_VERBOSE");

	return v && v[0] && strcmp(v, "0") != 0;
}

static void
rtu_cli_dump_stderr(void)
{
	FILE* file;
	char  line[512];

	if (!g_rtu_cli.stderr_path[0])
		return;
	file = fopen(g_rtu_cli.stderr_path, "r");
	if (!file)
		return;
	fprintf(stderr, "--- rtu cli stderr (%s) ---\n", g_rtu_cli.stderr_path);
	while (fgets(line, sizeof(line), file))
		fputs(line, stderr);
	fprintf(stderr, "--- end rtu cli stderr ---\n");
	fclose(file);
}

static void
rtu_cli_stop(void)
{
	rtu_cli_t* fx = &g_rtu_cli;
	pid_t pid = fx->pid;
	int status = 0;
	bool reaped = false;
	bool ok = true;

	fx->pid = -1;
	if (fx->master_fd >= 0)
	{
		close(fx->master_fd);
		fx->master_fd = -1;
	}
	if (pid > 0)
	{
		kill(pid, SIGTERM);
		for (u32 i = 0; i < 200; i++)
		{
			pid_t done = waitpid(pid, &status, WNOHANG);
			if (done == pid)
			{
				reaped = true;
				break;
			}
			if (done < 0 && errno == ECHILD)
			{
				ok = false;
				break;
			}
			usleep(10000);
		}
		if (!reaped && ok && waitpid(pid, &status, WNOHANG) != pid)
		{
			kill(pid, SIGKILL);
			reaped = waitpid(pid, &status, 0) == pid;
		}
		if (reaped)
			ok = WIFEXITED(status) ? WEXITSTATUS(status) == 0 : WIFSIGNALED(status);
	}
	if (!ok)
	{
		rtu_cli_dump_stderr();
		fprintf(stderr, "rtu cli: pid %d exited=%d status=%d signal=%d\n",
			(int)pid, WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
			WIFSIGNALED(status) ? WTERMSIG(status) : -1);
	}
	if (fx->config[0])
	{
		unlink(fx->config);
		fx->config[0] = '\0';
	}
	if (fx->stderr_path[0])
	{
		unlink(fx->stderr_path);
		fx->stderr_path[0] = '\0';
	}
	if (fx->stdout_path[0])
	{
		unlink(fx->stdout_path);
		fx->stdout_path[0] = '\0';
	}
	ASSERT_EQ(ok);
}

static bool
rtu_cli_write_config(const char* text)
{
	int fd;
	FILE* file;
	rtu_cli_t* fx = &g_rtu_cli;

	strcpy(fx->config, "/tmp/modbus-sim-rtucli-XXXXXX");
	fd = mkstemp(fx->config);
	if (fd < 0)
		return false;
	file = fdopen(fd, "w");
	if (!file)
	{
		close(fd);
		unlink(fx->config);
		fx->config[0] = '\0';
		return false;
	}
	fputs(text, file);
	fclose(file);
	return true;
}

static bool
rtu_cli_start_with_args(char* const argv[], bool capture_stderr)
{
	rtu_cli_t* fx = &g_rtu_cli;
	int fd;

	if (capture_stderr && !rtu_cli_verbose())
	{
		strcpy(fx->stderr_path, "/tmp/modbus-sim-rtuclierr-XXXXXX");
		fd = mkstemp(fx->stderr_path);
		if (fd < 0)
			return false;
		close(fd);
	}

	fx->pid = fork();
	if (fx->pid == 0)
	{
		if (!freopen("/dev/null", "r", stdin))
			_exit(127);
		if (!freopen("/dev/null", "w", stdout))
			_exit(127);
		if (capture_stderr && fx->stderr_path[0] && !rtu_cli_verbose())
		{
			if (!freopen(fx->stderr_path, "w", stderr))
				_exit(127);
			setvbuf(stderr, NULL, _IONBF, 0);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	if (fx->pid <= 0)
		return false;
	/* Give the server time to load config and enter the RTU loop. */
	usleep(300000);
	return kill(fx->pid, 0) == 0;
}

static bool
rtu_cli_start_with_args_capture_stdout(char* const argv[])
{
	rtu_cli_t* fx = &g_rtu_cli;
	int fd;

	if (!rtu_cli_verbose())
	{
		strcpy(fx->stderr_path, "/tmp/modbus-sim-rtuclierr-XXXXXX");
		fd = mkstemp(fx->stderr_path);
		if (fd < 0)
			return false;
		close(fd);
	}
	strcpy(fx->stdout_path, "/tmp/modbus-sim-rtucliout-XXXXXX");
	fd = mkstemp(fx->stdout_path);
	if (fd < 0)
		return false;
	close(fd);

	fx->pid = fork();
	if (fx->pid == 0)
	{
		if (!freopen("/dev/null", "r", stdin))
			_exit(127);
		if (!freopen(fx->stdout_path, "w", stdout))
			_exit(127);
		setvbuf(stdout, NULL, _IONBF, 0);
		if (fx->stderr_path[0] && !rtu_cli_verbose())
		{
			if (!freopen(fx->stderr_path, "w", stderr))
				_exit(127);
			setvbuf(stderr, NULL, _IONBF, 0);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	if (fx->pid <= 0)
		return false;
	usleep(300000);
	return kill(fx->pid, 0) == 0;
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
recv_rtu_resp_fd(int fd, u8* addr, u8* pdu, u32* pdu_len, u32 timeout_ms)
{
	u8 buf[MODBUS_RTU_MAX_FRAME];
	u32 got = 0;
	u64 deadline = clock_now_ms() + timeout_ms;

	while (got < sizeof(buf))
	{
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		u64 now = clock_now_ms();
		int wait_ms;
		ssize_t n;
		const u8* out_pdu = NULL;
		u32 out_len = 0;
		u8 out_addr = 0;

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

static bool
send_rtu_req_fd(int fd, u8 addr, const u8* pdu, u32 pdu_len)
{
	u8 frame[MODBUS_RTU_MAX_FRAME];
	u32 len = modbus_rtu_encode(frame, addr, pdu, pdu_len);

	if (len == 0)
		return false;
	return send_all_fd(fd, frame, len);
}

static bool
rtu_cli_start_pty(const char* config_text, const char* extra_flag, const char* extra_value)
{
	rtu_cli_t* fx = &g_rtu_cli;
	char* argv[10];
	int argc = 0;
	const char* bin = getenv("MODBUS_SIM_BIN");
	int slave = -1;
	char name[128];

	if (!bin)
		bin = "./modbus-sim";
	if (!rtu_cli_write_config(config_text))
		return false;
	if (openpty(&fx->master_fd, &slave, name, NULL, NULL) < 0)
		return false;
	{
		struct termios tio;
		if (tcgetattr(fx->master_fd, &tio) == 0)
		{
			cfmakeraw(&tio);
			(void)tcsetattr(fx->master_fd, TCSANOW, &tio);
		}
	}
	close(slave);
	/* Connect slave to a stable fd path via /dev/pts name. */
	argv[argc++] = (char*)bin;
	argv[argc++] = "--rtu-device";
	argv[argc++] = name;
	if (extra_flag)
		argv[argc++] = (char*)extra_flag;
	if (extra_value)
		argv[argc++] = (char*)extra_value;
	argv[argc++] = fx->config;
	argv[argc] = NULL;
	return rtu_cli_start_with_args(argv, true);
}

static bool
wait_for_process_exit(pid_t pid, int* status, u32 timeout_ms)
{
	u64 deadline = clock_now_ms() + timeout_ms;

	while (clock_now_ms() < deadline)
	{
		pid_t done = waitpid(pid, status, WNOHANG);
		if (done == pid)
			return true;
		if (done < 0 && errno == ECHILD)
			return true;
		usleep(10000);
	}
	return false;
}

TEST(RtuCliServesRtuOverPtyDevice)
{
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 resp[MODBUS_MAX_PDU];
	u8 addr = 0;
	u32 len = 0;
	bool ok = false;

	ASSERT_EQ(rtu_cli_start_pty("(device (unit-id 1) (holding 0 (static 4660)))\n", NULL, NULL));

	ok = send_rtu_req_fd(g_rtu_cli.master_fd, 1, req, sizeof(req));
	ok = ok && recv_rtu_resp_fd(g_rtu_cli.master_fd, &addr, resp, &len, 1000);
	ok = ok && addr == 1 && len == 4 && resp[0] == FC_READ_HOLDING_REGISTERS &&
	     get_be16(resp + 2) == 0x1234;

	rtu_cli_stop();
	ASSERT_EQ(ok);
}

TEST(RtuCliFrameGapOverrideAffectsResponseTiming)
{
	/* Use a large frame-gap override so a fragmented frame is not processed until gap elapses.
	 * With gap=300ms, send first half, wait 50ms (< gap), send rest: should not respond yet;
	 * after gap it should respond. This proves CLI --frame-gap-ms is wired to server. */
	u8 pdu[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME];
	u32 flen;
	u8 resp[MODBUS_MAX_PDU];
	u8 addr = 0;
	u32 len = 0;
	bool ok = false;
	u64 start;

	ASSERT_EQ(rtu_cli_start_pty("(server (frame-gap-ms 5))\n(device (unit-id 1) (holding 0 (static 7)))\n", "--frame-gap-ms", "300"));
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
		start = clock_now_ms();
		ok = send_all_fd(g_rtu_cli.master_fd, frame + flen / 2, flen - (flen / 2));
		ok = ok && recv_rtu_resp_fd(g_rtu_cli.master_fd, &addr, resp, &len, 1000);
		ok = ok && (clock_now_ms() - start) >= 200;
		ok = ok && addr == 1 && get_be16(resp + 2) == 7;
	}

	rtu_cli_stop();
	ASSERT_EQ(ok);
}

TEST(RtuCliRejectsTcpOnlyFlagsInRtuMode)
{
	const char* bin = getenv("MODBUS_SIM_BIN");
	char* argv1[] = { (char*)(bin ? bin : "./modbus-sim"), "--rtu-pty", "--port", "1502", g_rtu_cli.config, NULL };
	char* argv2[] = { (char*)(bin ? bin : "./modbus-sim"), "--rtu-pty", "--bind", "127.0.0.1", g_rtu_cli.config, NULL };
	int status = 0;
	bool ok;

	ASSERT_EQ(rtu_cli_write_config("(device (unit-id 1) (holding 0))\n"));
	ok = rtu_cli_start_with_args(argv1, false);
	ASSERT_EQ(ok);
	ASSERT_EQ(wait_for_process_exit(g_rtu_cli.pid, &status, 2000));
	ASSERT_EQ(WIFEXITED(status) && WEXITSTATUS(status) == 2);
	g_rtu_cli.pid = -1;

	ok = rtu_cli_start_with_args(argv2, false);
	ASSERT_EQ(ok);
	ASSERT_EQ(wait_for_process_exit(g_rtu_cli.pid, &status, 2000));
	ASSERT_EQ(WIFEXITED(status) && WEXITSTATUS(status) == 2);
	g_rtu_cli.pid = -1;
	rtu_cli_stop();
}

static bool
rtu_cli_file_contains(const char* path, const char* needle)
{
	FILE* file = fopen(path, "r");
	char line[512];
	bool found = false;

	if (!file)
		return false;
	while (fgets(line, sizeof(line), file))
	{
		if (strstr(line, needle))
		{
			found = true;
			break;
		}
	}
	fclose(file);
	return found;
}

static bool
rtu_cli_stderr_contains(const char* needle)
{
	return rtu_cli_file_contains(g_rtu_cli.stderr_path, needle);
}

TEST(RtuCliRejectsNonStandardBaud)
{
	const char* bin = getenv("MODBUS_SIM_BIN");
	char* argv[] = { (char*)(bin ? bin : "./modbus-sim"), "--rtu-pty", "--baud", "12345", g_rtu_cli.config, NULL };
	int status = 0;

	ASSERT_EQ(rtu_cli_write_config("(device (unit-id 1) (holding 0))\n"));
	ASSERT_EQ(rtu_cli_start_with_args(argv, true));
	ASSERT_EQ(wait_for_process_exit(g_rtu_cli.pid, &status, 2000));
	ASSERT_EQ(WIFEXITED(status) && WEXITSTATUS(status) == 2);
	ASSERT_EQ(rtu_cli_stderr_contains("standard termios rate"));
	g_rtu_cli.pid = -1;
	rtu_cli_stop();
}

TEST(RtuCliCheckRejectsNonStandardBaud)
{
	const char* bin = getenv("MODBUS_SIM_BIN");
	char* argv[] = { (char*)(bin ? bin : "./modbus-sim"), "--check", "--rtu-pty", "--baud", "12345", g_rtu_cli.config, NULL };
	int status = 0;

	ASSERT_EQ(rtu_cli_write_config("(device (unit-id 1) (holding 0))\n"));
	ASSERT_EQ(rtu_cli_start_with_args(argv, true));
	ASSERT_EQ(wait_for_process_exit(g_rtu_cli.pid, &status, 2000));
	ASSERT_EQ(WIFEXITED(status) && WEXITSTATUS(status) == 2);
	ASSERT_EQ(rtu_cli_stderr_contains("standard termios rate"));
	g_rtu_cli.pid = -1;
	rtu_cli_stop();
}

TEST(RtuCliRejectsReservedUnitIdsInRtuMode)
{
	const char* bin = getenv("MODBUS_SIM_BIN");
	char* argv0[] = { (char*)(bin ? bin : "./modbus-sim"), "--rtu-pty", g_rtu_cli.config, NULL };
	char* argv248[] = { (char*)(bin ? bin : "./modbus-sim"), "--rtu-device", "/dev/null", g_rtu_cli.config, NULL };
	char* argv255[] = { (char*)(bin ? bin : "./modbus-sim"), "--rtu-pty", g_rtu_cli.config, NULL };
	int status = 0;
	bool ok;

	ASSERT_EQ(rtu_cli_write_config("(device (unit-id 0) (holding 0))\n"));
	ok = rtu_cli_start_with_args(argv0, true);
	ASSERT_EQ(ok);
	ASSERT_EQ(wait_for_process_exit(g_rtu_cli.pid, &status, 2000));
	ASSERT_EQ(WIFEXITED(status) && WEXITSTATUS(status) == 2);
	ASSERT_EQ(rtu_cli_file_contains(g_rtu_cli.stderr_path, "not valid for RTU"));
	g_rtu_cli.pid = -1;
	rtu_cli_stop();

	ASSERT_EQ(rtu_cli_write_config("(device (unit-id 248) (holding 0))\n"));
	ok = rtu_cli_start_with_args(argv248, true);
	ASSERT_EQ(ok);
	ASSERT_EQ(wait_for_process_exit(g_rtu_cli.pid, &status, 2000));
	ASSERT_EQ(WIFEXITED(status) && WEXITSTATUS(status) == 2);
	ASSERT_EQ(rtu_cli_file_contains(g_rtu_cli.stderr_path, "not valid for RTU"));
	g_rtu_cli.pid = -1;
	rtu_cli_stop();

	ASSERT_EQ(rtu_cli_write_config("(device (unit-id 255) (holding 0))\n"));
	ok = rtu_cli_start_with_args(argv255, true);
	ASSERT_EQ(ok);
	ASSERT_EQ(wait_for_process_exit(g_rtu_cli.pid, &status, 2000));
	ASSERT_EQ(WIFEXITED(status) && WEXITSTATUS(status) == 2);
	ASSERT_EQ(rtu_cli_file_contains(g_rtu_cli.stderr_path, "not valid for RTU"));
	g_rtu_cli.pid = -1;
	rtu_cli_stop();
}

TEST(RtuCliCheckSkipsReservedUnitIdWarningWithoutRtuMode)
{
	const char* bin = getenv("MODBUS_SIM_BIN");
	char* argv[] = { (char*)(bin ? bin : "./modbus-sim"), "--check", g_rtu_cli.config, NULL };
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

TEST(RtuCliCheckRejectsReservedUnitIdsInRtuMode)
{
	const char* bin = getenv("MODBUS_SIM_BIN");
	char* argv[] = { (char*)(bin ? bin : "./modbus-sim"), "--check", "--rtu-pty", g_rtu_cli.config, NULL };
	int status = 0;
	bool ok;

	ASSERT_EQ(rtu_cli_write_config("(device (unit-id 248) (holding 0))\n"));
	ok = rtu_cli_start_with_args(argv, true);
	ASSERT_EQ(ok);
	ASSERT_EQ(wait_for_process_exit(g_rtu_cli.pid, &status, 2000));
	ASSERT_EQ(WIFEXITED(status) && WEXITSTATUS(status) == 2);
	ASSERT_EQ(rtu_cli_file_contains(g_rtu_cli.stderr_path, "not valid for RTU"));
	g_rtu_cli.pid = -1;
	rtu_cli_stop();
}

TEST(RtuCliPtyModePrintsSlavePathAndServesRequests)
{
	const char* bin = getenv("MODBUS_SIM_BIN");
	int outpipe[2] = { -1, -1 };
	pid_t pid;
	int status = 0;
	char line[256];
	ssize_t n;
	char slave_path[128] = "";
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 resp[MODBUS_MAX_PDU];
	u8 addr = 0;
	u32 len = 0;
	bool ok = false;
	int slave_fd = -1;

	if (!bin)
		bin = "./modbus-sim";
	ASSERT_EQ(rtu_cli_write_config("(device (unit-id 1) (holding 0 (static 4660)))\n"));
	ASSERT_EQ(pipe(outpipe) == 0);

	pid = fork();
	if (pid == 0)
	{
		char* argv[] = { (char*)bin, "--rtu-pty", "--frame-gap-ms", "5", g_rtu_cli.config, NULL };
		close(outpipe[0]);
		dup2(outpipe[1], STDOUT_FILENO);
		close(outpipe[1]);
		if (!freopen("/dev/null", "r", stdin))
			_exit(127);
		if (!freopen("/dev/null", "w", stderr))
			_exit(127);
		execv(argv[0], argv);
		_exit(127);
	}
	close(outpipe[1]);
	outpipe[1] = -1;
	ASSERT_EQ(pid > 0);

	{
		u32 got = 0;
		u64 deadline = clock_now_ms() + 2000;
		while (clock_now_ms() < deadline && got < sizeof(line) - 1)
		{
			struct pollfd pfd = { .fd = outpipe[0], .events = POLLIN };
			int rc = poll(&pfd, 1, 50);
			if (rc > 0 && (pfd.revents & POLLIN))
			{
				n = read(outpipe[0], line + got, sizeof(line) - 1 - got);
				if (n > 0)
				{
					char* nl;
					got += (u32)n;
					line[got] = '\0';
					nl = strchr(line, '\n');
					while (nl)
					{
						*nl = '\0';
						if (sscanf(line, "RTU PTY slave: %127s", slave_path) == 1)
							goto got_path;
						{
							u32 rest = got - (u32)(nl - line + 1);
							memmove(line, nl + 1, rest + 1);
							got = rest;
						}
						nl = strchr(line, '\n');
					}
				}
			}
		}
got_path:
		;
	}

	if (slave_path[0])
	{
		slave_fd = open(slave_path, O_RDWR | O_NOCTTY);
		if (slave_fd >= 0)
		{
			struct termios tio;
			if (tcgetattr(slave_fd, &tio) == 0)
			{
				cfmakeraw(&tio);
				(void)tcsetattr(slave_fd, TCSANOW, &tio);
			}
			ok = send_rtu_req_fd(slave_fd, 1, req, sizeof(req));
			ok = ok && recv_rtu_resp_fd(slave_fd, &addr, resp, &len, 1500);
			ok = ok && addr == 1 && len == 4 && get_be16(resp + 2) == 0x1234;
		}
	}

	if (slave_fd >= 0)
		close(slave_fd);
	close(outpipe[0]);
	kill(pid, SIGTERM);
	wait_for_process_exit(pid, &status, 2000);
	if (waitpid(pid, &status, WNOHANG) != pid)
	{
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
	}
	rtu_cli_stop(); /* cleans config/stderr paths; pid/master already handled */
	ASSERT_EQ(slave_path[0] != '\0');
	ASSERT_EQ(ok);
}


TEST(RtuCliDeviceOpenDoesNotBlockOnFifoWithoutWriter)
{
	const char* bin = getenv("MODBUS_SIM_BIN");
	char fifo_path[] = "/tmp/modbus-sim-rtu-fifo-XXXXXX";
	char* argv[8];
	int argc = 0;
	int fd;
	pid_t pid;
	int status = 0;
	bool ok;

	if (!bin)
		bin = "./modbus-sim";
	ASSERT_EQ(rtu_cli_write_config("(device (unit-id 1) (holding 0 (static 1)))\n"));
	fd = mkstemp(fifo_path);
	ASSERT_EQ(fd >= 0);
	close(fd);
	unlink(fifo_path);
	ASSERT_EQ(mkfifo(fifo_path, 0600) == 0);

	argv[argc++] = (char*)bin;
	argv[argc++] = "--rtu-device";
	argv[argc++] = fifo_path;
	argv[argc++] = g_rtu_cli.config;
	argv[argc] = NULL;

	pid = fork();
	if (pid == 0)
	{
		if (!freopen("/dev/null", "r", stdin))
			_exit(127);
		if (!freopen("/dev/null", "w", stdout))
			_exit(127);
		if (!freopen("/dev/null", "w", stderr))
			_exit(127);
		execv(argv[0], argv);
		_exit(127);
	}
	ASSERT_EQ(pid > 0);

	/* The process should enter the RTU poll loop promptly; a blocking open()
	 * would hang here until a writer opens the FIFO. A FIFO opened O_RDWR has
	 * no EOF on Linux, so terminate the process after confirming it started. */
	usleep(300000);
	ok = (kill(pid, 0) == 0);
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
	const char* bin = getenv("MODBUS_SIM_BIN");
	int master_fd = -1;
	int slave_fd = -1;
	char name[128];
	struct termios tio;
	struct termios after;
	pid_t pid;
	int status = 0;
	bool ok = false;

	if (!bin)
		bin = "./modbus-sim";
	ASSERT_EQ(rtu_cli_write_config("(device (unit-id 1) (holding 0 (static 1)))\n"));
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

	pid = fork();
	if (pid == 0)
	{
		char* argv[] = { (char*)bin, "--rtu-device", name, g_rtu_cli.config, NULL };
		close(master_fd);
		close(slave_fd);
		if (!freopen("/dev/null", "r", stdin))
			_exit(127);
		if (!freopen("/dev/null", "w", stdout))
			_exit(127);
		if (!freopen("/dev/null", "w", stderr))
			_exit(127);
		execv(argv[0], argv);
		_exit(127);
	}
	ASSERT_EQ(pid > 0);

	/* Give server time to open and configure the device. */
	usleep(300000);
	if (kill(pid, 0) == 0)
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
rtu_cli_start_pty_extra(const char* config_text, char* const extra[], u32 extra_count)
{
	rtu_cli_t* fx = &g_rtu_cli;
	char* argv[16];
	int argc = 0;
	const char* bin = getenv("MODBUS_SIM_BIN");
	int slave = -1;
	char name[128];

	if (!bin)
		bin = "./modbus-sim";
	if (extra_count > 8 || !rtu_cli_write_config(config_text))
		return false;
	if (openpty(&fx->master_fd, &slave, name, NULL, NULL) < 0)
		return false;
	{
		struct termios tio;
		if (tcgetattr(fx->master_fd, &tio) == 0)
		{
			cfmakeraw(&tio);
			(void)tcsetattr(fx->master_fd, TCSANOW, &tio);
		}
	}
	close(slave);
	argv[argc++] = (char*)bin;
	argv[argc++] = "--rtu-device";
	argv[argc++] = name;
	for (u32 i = 0; i < extra_count; i++)
		argv[argc++] = extra[i];
	argv[argc++] = fx->config;
	argv[argc] = NULL;
	return rtu_cli_start_with_args(argv, true);
}

TEST(RtuCliBaudAffectsRtuTiming)
{
	/* 9600 baud -> t3.5 ~4.01ms, effective 5ms. CLI --baud must make the server
	 * treat byte gaps larger than t3.5 as frame boundaries. */
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME];
	u8 resp[MODBUS_MAX_PDU];
	u8 addr = 0;
	u32 len = 0;
	u32 flen = 0;
	char* extra[] = { "--baud", "9600" };
	bool ok = true;

	ASSERT_EQ(rtu_cli_start_pty_extra(
		"(device (unit-id 1) (holding 0 (static 4660)))\n", extra, 2));
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

TEST(RtuCliFrameGapOverrideBeatsBaud)
{
	/* Baud-derived t3.5 at 9600 is ~4ms, but explicit --frame-gap-ms override must win. */
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME];
	u8 resp[MODBUS_MAX_PDU];
	u8 addr = 0;
	u32 len = 0;
	u32 flen = 0;
	char* extra[] = { "--baud", "9600", "--frame-gap-ms", "80" };
	bool ok = false;

	ASSERT_EQ(rtu_cli_start_pty_extra(
		"(device (unit-id 1) (holding 0 (static 4660)))\n", extra, 4));
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

static bool
rtu_cli_start_real_pty(const char* config_text, char* const extra[], u32 extra_count)
{
	rtu_cli_t* fx = &g_rtu_cli;
	char* argv[16];
	int argc = 0;
	const char* bin = getenv("MODBUS_SIM_BIN");

	if (!bin)
		bin = "./modbus-sim";
	if (extra_count > 8 || !rtu_cli_write_config(config_text))
		return false;
	argv[argc++] = (char*)bin;
	argv[argc++] = "--rtu-pty";
	for (u32 i = 0; i < extra_count; i++)
		argv[argc++] = extra[i];
	argv[argc++] = fx->config;
	argv[argc] = NULL;
	return rtu_cli_start_with_args_capture_stdout(argv);
}

TEST(RtuCliPtyDefaultUsesUnknownBaudFallback)
{
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 frame[MODBUS_RTU_MAX_FRAME];
	u8 resp[MODBUS_MAX_PDU];
	u8 addr = 0;
	u32 len = 0;
	u32 flen = 0;
	char slave_path[128];
	bool ok = false;

	/* Real --rtu-pty path: #42 regression (master fd must not auto-detect 38400). */
	ASSERT_EQ(rtu_cli_start_real_pty("(device (unit-id 1) (holding 0 (static 4660)))\n", NULL, 0));
	ASSERT_EQ(rtu_cli_stderr_contains("frame gap 20 ms (default; baud unknown)"));
	{
		FILE* out = fopen(g_rtu_cli.stdout_path, "r");
		char line[512];
		bool found = false;
		slave_path[0] = '\0';
		ASSERT(out != NULL);
		while (fgets(line, sizeof(line), out))
		{
			char* p = strstr(line, "RTU PTY slave: ");
			if (!p) continue;
			p += strlen("RTU PTY slave: ");
			snprintf(slave_path, sizeof(slave_path), "%.*s", (int)strcspn(p, "\r\n"), p);
			found = slave_path[0] != '\0';
			break;
		}
		fclose(out);
		ASSERT_EQ(found);
	}
	g_rtu_cli.master_fd = open(slave_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
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

TEST(RtuCliBaudOverrideOnPtyLogsDerivedTiming)
{
	char* extra[] = { "--baud", "9600" };

	ASSERT_EQ(rtu_cli_start_real_pty(
		"(device (unit-id 1) (holding 0 (static 4660)))\n", extra, 2));
	ASSERT_EQ(rtu_cli_stderr_contains("baud 9600, t3.5 4011 us (frame gap 5 ms)"));
	rtu_cli_stop();
}
