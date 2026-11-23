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

#define RTU_CLI_START_TIMEOUT_MS	5000

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
		if (!reaped &&
		    ok &&
		    waitpid(pid, &status, WNOHANG) != pid)
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
rtu_cli_capture_stderr(void)
{
	int fd;

	strcpy(g_rtu_cli.stderr_path, "/tmp/modbus-sim-rtuclierr-XXXXXX");
	fd = mkstemp(g_rtu_cli.stderr_path);
	if (fd < 0)
	{
		g_rtu_cli.stderr_path[0] = '\0';
		return false;
	}
	close(fd);
	return true;
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

/* WNOWAIT notices an early exit without reaping it: callers still waitpid(). */
static bool
rtu_cli_process_done(pid_t pid)
{
	siginfo_t info = { 0 };

	return waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
	       info.si_pid == pid;
}

/* A server logs "listening on" immediately before entering its poll loop; short-lived
 * --check/invalid-flag runs are detected by their exit instead. */
static bool
rtu_cli_wait_startup(pid_t pid)
{
	bool have_log = g_rtu_cli.stderr_path[0] != '\0';
	/* Without a log file (verbose runs, or callers not capturing stderr) there is
	 * no readiness signal for a server, so keep the short historical startup wait. */
	u64 deadline = clock_now_ms() + (have_log ? RTU_CLI_START_TIMEOUT_MS : 300);

	for (;;)
	{
		if (have_log && rtu_cli_file_contains(g_rtu_cli.stderr_path, "listening on"))
			return kill(pid, 0) == 0;
		if (rtu_cli_process_done(pid))
			return kill(pid, 0) == 0;
		if (clock_now_ms() >= deadline)
		{
			if (!have_log)
				return kill(pid, 0) == 0;
			fprintf(stderr, "rtu cli: pid %d never became ready\n", (int)pid);
			rtu_cli_dump_stderr();
			return false;
		}
		usleep(2000);
	}
}

static bool
rtu_cli_start_with_args(char* const argv[], bool capture_stderr)
{
	rtu_cli_t* fx = &g_rtu_cli;

	if (capture_stderr &&
	    !rtu_cli_verbose() &&
	    !rtu_cli_capture_stderr())
		return false;

	fx->pid = fork();
	if (fx->pid == 0)
	{
		if (!freopen("/dev/null", "r", stdin))
			_exit(127);
		if (!freopen("/dev/null", "w", stdout))
			_exit(127);
		if (capture_stderr &&
		    fx->stderr_path[0] &&
		    !rtu_cli_verbose())
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
	return rtu_cli_wait_startup(fx->pid);
}

static bool
rtu_cli_start_with_args_capture_stdout(char* const argv[])
{
	rtu_cli_t* fx = &g_rtu_cli;
	int fd;

	if (!rtu_cli_verbose() && !rtu_cli_capture_stderr())
		return false;
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
	return rtu_cli_wait_startup(fx->pid);
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
recv_none_fd(int fd, u32 timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int rc = poll(&pfd, 1, (int)timeout_ms);

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
send_rtu_req_fd(int fd, u8 addr, const u8* pdu, u32 pdu_len)
{
	u8 frame[MODBUS_RTU_MAX_FRAME];
	u32 len = modbus_rtu_encode(frame, addr, pdu, pdu_len);

	if (len == 0)
		return false;
	return send_all_fd(fd, frame, len);
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
	rtu_cli_t* fx = &g_rtu_cli;
	char* argv[8];
	int argc = 0;
	const char* bin = getenv("MODBUS_SIM_BIN");
	int slave = -1;
	char name[128];

	if (!bin)
		bin = "./modbus-sim";
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
	if (!rtu_cli_write_server_config("rtu-device", name, options, devices))
		return false;
	argv[argc++] = (char*)bin;
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

static bool
rtu_cli_stderr_contains(const char* needle)
{
	return rtu_cli_file_contains(g_rtu_cli.stderr_path, needle);
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
		start = clock_now_ms();
		ok = send_all_fd(g_rtu_cli.master_fd, frame + flen / 2, flen - (flen / 2));
		ok = ok && recv_rtu_resp_fd(g_rtu_cli.master_fd, &addr, resp, &len, 1000);
		ok = ok && (clock_now_ms() - start) >= 200;
		ok = ok && addr == 1 && get_be16(resp + 2) == 7;
	}

	rtu_cli_stop();
	ASSERT_EQ(ok);
}

TEST(RtuCliCheckSkipsReservedUnitIdWarningWithoutSerialMode)
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
	ASSERT_EQ(rtu_cli_write_server_config("rtu-pty", NULL, "(frame-gap-ms 5)",
					      "(device (unit-id 1) (holding 0 (static 4660)))\n"));
	ASSERT_EQ(pipe(outpipe) == 0);

	pid = fork();
	if (pid == 0)
	{
		char* argv[] = { (char*)bin, g_rtu_cli.config, NULL };
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
	const char* bin = getenv("MODBUS_SIM_BIN");
	int master_fd = -1;
	int slave_fd = -1;
	char name[128];
	struct termios tio;
	pid_t pid;
	int status = 0;
	bool ok = false;

	if (!bin)
		bin = "./modbus-sim";
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

TEST(RtuCliDefaultLineFlagsAre8N1)
{
	struct termios after;

	ASSERT_EQ(rtu_cli_pty_line_flags("", "(device (unit-id 1) (holding 0))\n", &after));
	ASSERT_EQ(rtu_cli_line_flags_match(&after, 0));
}

TEST(RtuCliEvenParityFromConfig)
{
	struct termios after;

	ASSERT_EQ(rtu_cli_pty_line_flags("(parity even)",
					 "(device (unit-id 1) (holding 0))\n", &after));
	ASSERT_EQ(rtu_cli_line_flags_match(&after, PARENB));
}

TEST(RtuCliOddParityFromConfig)
{
	struct termios after;

	ASSERT_EQ(rtu_cli_pty_line_flags("(parity odd)",
					 "(device (unit-id 1) (holding 0))\n", &after));
	ASSERT_EQ(rtu_cli_line_flags_match(&after, PARENB | PARODD));
}

#ifdef CMSPAR
TEST(RtuCliMarkAndSpaceParityFlags)
{
	struct termios after;

	ASSERT_EQ(rtu_cli_pty_line_flags("(parity mark)",
					 "(device (unit-id 1) (holding 0))\n", &after));
	ASSERT_EQ(rtu_cli_line_flags_match(&after, PARENB | PARODD | CMSPAR));

	ASSERT_EQ(rtu_cli_pty_line_flags("(parity space)",
					 "(device (unit-id 1) (holding 0))\n", &after));
	ASSERT_EQ(rtu_cli_line_flags_match(&after, PARENB | CMSPAR));
}
#endif

TEST(RtuCliStopBitsFromConfig)
{
	struct termios after;

	ASSERT_EQ(rtu_cli_pty_line_flags("(stop-bits 2)",
					 "(device (unit-id 1) (holding 0))\n", &after));
	ASSERT_EQ(rtu_cli_line_flags_match(&after, CSTOPB));

	ASSERT_EQ(rtu_cli_pty_line_flags("(stop-bits 1)",
					 "(device (unit-id 1) (holding 0))\n", &after));
	ASSERT_EQ(rtu_cli_line_flags_match(&after, 0));
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

static bool
rtu_cli_start_real_pty(const char* options)
{
	rtu_cli_t* fx = &g_rtu_cli;
	char* argv[8];
	int argc = 0;
	const char* bin = getenv("MODBUS_SIM_BIN");

	if (!bin)
		bin = "./modbus-sim";
	if (!rtu_cli_write_server_config("rtu-pty", NULL, options,
					 "(device (unit-id 1) (holding 0 (static 4660)))\n"))
		return false;
	argv[argc++] = (char*)bin;
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

	/* Real rtu-pty config path: #42 regression (master fd must not auto-detect 38400). */
	ASSERT_EQ(rtu_cli_start_real_pty(""));
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

TEST(RtuCliConfigBaudOnPtyLogsDerivedTiming)
{
	ASSERT_EQ(rtu_cli_start_real_pty("(baud 9600)"));
	ASSERT_EQ(rtu_cli_stderr_contains("baud 9600, t3.5 4011 us (frame gap 5 ms)"));
	rtu_cli_stop();
}

TEST(RtuCliCheckRtuTcpPrintsTimingSummary)
{
	const char* bin = getenv("MODBUS_SIM_BIN");
	char* argv[] = { (char*)(bin ? bin : "./modbus-sim"), "--check", g_rtu_cli.config, NULL };
	int status = 0;
	bool ok;

	ASSERT_EQ(rtu_cli_write_server_config("rtu-tcp", NULL, "(baud 9600)",
					      "(device (unit-id 1) (holding 0))\n"));
	ok = rtu_cli_start_with_args_capture_stdout(argv);
	ASSERT_EQ(ok);
	ASSERT_EQ(wait_for_process_exit(g_rtu_cli.pid, &status, 2000));
	ASSERT_EQ(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	ASSERT_EQ(rtu_cli_file_contains(g_rtu_cli.stdout_path, "RTU-over-TCP"));
	ASSERT_EQ(rtu_cli_file_contains(g_rtu_cli.stdout_path,
					"baud 9600, t3.5 4011 us (frame-gap 5 ms)"));
	ASSERT_EQ(rtu_cli_file_contains(g_rtu_cli.stdout_path, "config OK"));
	g_rtu_cli.pid = -1;
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

TEST(RtuCliCheckRtuTcpBroadcastExecutePrintsSummary)
{
	const char* bin = getenv("MODBUS_SIM_BIN");
	char* argv[] = { (char*)(bin ? bin : "./modbus-sim"), "--check", g_rtu_cli.config, NULL };
	int status = 0;

	ASSERT_EQ(rtu_cli_write_server_config("rtu-tcp", NULL, "(broadcast-execute on)",
					      "(device (unit-id 1) (holding 0))\n"));
	ASSERT_EQ(rtu_cli_start_with_args_capture_stdout(argv));
	ASSERT_EQ(wait_for_process_exit(g_rtu_cli.pid, &status, 2000));
	ASSERT_EQ(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	ASSERT_EQ(rtu_cli_file_contains(g_rtu_cli.stdout_path, "RTU-over-TCP"));
	ASSERT_EQ(rtu_cli_file_contains(g_rtu_cli.stdout_path, "broadcast-execute on"));
	g_rtu_cli.pid = -1;
	rtu_cli_stop();
}

TEST(RtuCliCheckTcpIgnoresSerialLineConfig)
{
	const char* bin = getenv("MODBUS_SIM_BIN");
	char* argv[] = { (char*)(bin ? bin : "./modbus-sim"), "--check", g_rtu_cli.config, NULL };
	int status = 0;

	ASSERT_EQ(rtu_cli_write_config("(server (parity even) (stop-bits 2))\n"
				       "(device (unit-id 1) (holding 0))\n"));
	ASSERT_EQ(rtu_cli_start_with_args(argv, true));
	ASSERT_EQ(wait_for_process_exit(g_rtu_cli.pid, &status, 2000));
	ASSERT_EQ(WIFEXITED(status) && WEXITSTATUS(status) == 0);
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
rtu_cli_expect_cli_success(char* const argv[], const char* stdout_needle)
{
	int status = 0;
	bool ok = rtu_cli_start_with_args_capture_stdout(argv);

	ok = ok && wait_for_process_exit(g_rtu_cli.pid, &status, 2000);
	ok = ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
	ok = ok && (!stdout_needle || rtu_cli_file_contains(g_rtu_cli.stdout_path, stdout_needle));
	g_rtu_cli.pid = -1;
	rtu_cli_stop();
	return ok;
}

TEST(RtuCliCheckSummarizesConfiguredModes)
{
	const char* bin = getenv("MODBUS_SIM_BIN");
	const char devices[] = "(device (unit-id 1) (holding 0 (static 1)))\n";
	bool ok = true;

	if (!bin)
		bin = "./modbus-sim";

	{
		char* argv[] = { (char*)bin, "--check", g_rtu_cli.config, NULL };

		ASSERT_EQ(rtu_cli_write_server_config("rtu-device", "/dev/ttyS0", "", devices));
		ok = ok && rtu_cli_expect_cli_success(argv, "RTU /dev/ttyS0");
	}
	{
		char* argv[] = { (char*)bin, "--check", g_rtu_cli.config, NULL };

		ASSERT_EQ(rtu_cli_write_server_config("ascii-device", "/dev/ttyUSB0", "", devices));
		ok = ok && rtu_cli_expect_cli_success(argv, "ASCII /dev/ttyUSB0");
	}
	{
		char* argv[] = { (char*)bin, "--check", g_rtu_cli.config, NULL };

		ASSERT_EQ(rtu_cli_write_server_config("rtu-pty", NULL, "", devices));
		ok = ok && rtu_cli_expect_cli_success(argv, "RTU pty");
	}
	ASSERT_EQ(ok);
}

TEST(RtuCliAcceptsOnlyGenericOptionsAndRejectsServerFlags)
{
	const char* bin = getenv("MODBUS_SIM_BIN");
	static const char* const removed_flags[] = {
		"-p", "--port", "-b", "--bind", "--rtu-pty", "--rtu-device",
		"--rtu-tcp", "--ascii-pty", "--ascii-device", "--baud", "--rtu-baud",
		"--frame-gap-ms", "--strict-t15", "--broadcast-execute",
		"--parity", "--stop-bits"
	};
	const char valid[] = "(device (unit-id 1) (holding 0 (static 1)))\n";
	bool ok = true;

	if (!bin)
		bin = "./modbus-sim";

	for (u32 i = 0; i < sizeof(removed_flags) / sizeof(removed_flags[0]); i++)
	{
		char* argv[] = { (char*)bin, (char*)removed_flags[i], (char*)"x",
				 g_rtu_cli.config, NULL };

		ASSERT_EQ(rtu_cli_write_config(valid));
		ok = ok && rtu_cli_expect_cli_error(argv, "unknown option");
	}

	{
		char* argv[] = { (char*)bin, "--help", NULL };

		ok = ok && rtu_cli_expect_cli_success(argv, "defined in (server ...)");
	}
	{
		char* argv[] = { (char*)bin, "-h", NULL };

		ok = ok && rtu_cli_expect_cli_success(argv, "Usage: modbus-sim");
	}
	{
		char* argv[] = { (char*)bin, "--check", g_rtu_cli.config, NULL };

		ASSERT_EQ(rtu_cli_write_config(valid));
		ok = ok && rtu_cli_expect_cli_success(argv, "config OK");
	}
	{
		char* argv[] = { (char*)bin, "--verbose", "-c", g_rtu_cli.config, NULL };

		ASSERT_EQ(rtu_cli_write_config(valid));
		ok = ok && rtu_cli_expect_cli_success(argv, "config OK");
	}
	{
		char* argv[] = { (char*)bin, "-v", "--check", g_rtu_cli.config, NULL };

		ASSERT_EQ(rtu_cli_write_config(valid));
		ok = ok && rtu_cli_expect_cli_success(argv, "config OK");
	}
	ASSERT_EQ(ok);
}
