#define _GNU_SOURCE
#include "vtest.h"
#include "modbus/ascii.h"
#include "modbus/bytes.h"
#include "modbus/pdu.h"
#include "util/clock.h"
#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define ASCII_CLI_START_TIMEOUT_MS	5000
#define ASCII_CLI_IO_TIMEOUT_MS		1500

typedef struct ascii_cli_t ascii_cli_t;
struct ascii_cli_t
{
	pid_t	pid;
	int	master_fd;
	char	config[80];
	char	stderr_path[80];
};

static ascii_cli_t g_ascii_cli = { .pid = -1, .master_fd = -1 };

static bool
ascii_cli_verbose(void)
{
	const char* v = getenv("MODBUS_SIM_TEST_VERBOSE");

	return v && v[0] && strcmp(v, "0") != 0;
}

static void
ascii_cli_dump_stderr(void)
{
	FILE* file;
	char line[512];

	if (!g_ascii_cli.stderr_path[0])
		return;
	file = fopen(g_ascii_cli.stderr_path, "r");
	if (!file)
		return;
	fprintf(stderr, "--- ascii cli stderr (%s) ---\n", g_ascii_cli.stderr_path);
	while (fgets(line, sizeof(line), file))
		fputs(line, stderr);
	fprintf(stderr, "--- end ascii cli stderr ---\n");
	fclose(file);
}

static void
ascii_cli_stop(void)
{
	ascii_cli_t* fx = &g_ascii_cli;
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
		ascii_cli_dump_stderr();
		fprintf(stderr, "ascii cli: pid %d exited=%d status=%d signal=%d\n",
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
	ASSERT_EQ(ok);
}

static bool
ascii_cli_write_config(const char* text)
{
	ascii_cli_t* fx = &g_ascii_cli;
	int fd;
	FILE* file;

	strcpy(fx->config, "/tmp/modbus-sim-asciicli-XXXXXX");
	fd = mkstemp(fx->config);
	if (fd < 0)
	{
		fx->config[0] = '\0';
		return false;
	}
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
ascii_cli_capture_stderr(void)
{
	int fd;

	strcpy(g_ascii_cli.stderr_path, "/tmp/modbus-sim-asciiclierr-XXXXXX");
	fd = mkstemp(g_ascii_cli.stderr_path);
	if (fd < 0)
	{
		g_ascii_cli.stderr_path[0] = '\0';
		return false;
	}
	close(fd);
	return true;
}

static bool
ascii_cli_file_contains(const char* path, const char* needle)
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
ascii_cli_process_done(pid_t pid)
{
	siginfo_t info = { 0 };

	return waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
	       info.si_pid == pid;
}

static bool
ascii_cli_wait_startup(pid_t pid)
{
	bool have_log = g_ascii_cli.stderr_path[0] != '\0';
	u64 deadline = clock_now_ms() + (have_log ? ASCII_CLI_START_TIMEOUT_MS : 300);

	for (;;)
	{
		if (have_log && ascii_cli_file_contains(g_ascii_cli.stderr_path, "ASCII listening on"))
			return kill(pid, 0) == 0;
		if (ascii_cli_process_done(pid))
			return kill(pid, 0) == 0;
		if (clock_now_ms() >= deadline)
		{
			if (!have_log)
				return kill(pid, 0) == 0;
			fprintf(stderr, "ascii cli: pid %d never became ready\n", (int)pid);
			ascii_cli_dump_stderr();
			return false;
		}
		usleep(2000);
	}
}

static bool
ascii_cli_start_with_args(char* const argv[])
{
	pid_t pid = fork();

	if (pid == 0)
	{
		if (!freopen("/dev/null", "r", stdin))
			_exit(127);
		if (!freopen("/dev/null", "w", stdout))
			_exit(127);
		if (g_ascii_cli.stderr_path[0] && !ascii_cli_verbose())
		{
			if (!freopen(g_ascii_cli.stderr_path, "w", stderr))
				_exit(127);
			setvbuf(stderr, NULL, _IONBF, 0);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	if (pid <= 0)
		return false;
	g_ascii_cli.pid = pid;
	return ascii_cli_wait_startup(pid);
}

static bool
ascii_cli_start_device_with_options(const char* options, const char* devices, bool dirty_line)
{
	const char* bin = getenv("MODBUS_SIM_BIN");
	char config[1024];
	char* argv[4];
	int slave = -1;
	char name[128];
	struct termios tio;

	if (!bin)
		bin = "./modbus-sim";
	if (openpty(&g_ascii_cli.master_fd, &slave, name, NULL, NULL) < 0)
		return false;
	if (tcgetattr(g_ascii_cli.master_fd, &tio) == 0)
	{
		cfmakeraw(&tio);
		cfsetispeed(&tio, B19200);
		cfsetospeed(&tio, B19200);
		if (dirty_line)
		{
			tio.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
#ifdef CMSPAR
			tio.c_cflag &= ~CMSPAR;
#endif
			tio.c_cflag |= CS7 | PARENB | PARODD | CSTOPB | CRTSCTS;
		}
		(void)tcsetattr(g_ascii_cli.master_fd, TCSANOW, &tio);
	}
	close(slave);
	snprintf(config, sizeof(config), "(server (mode ascii-device) (path \"%s\")%s%s)\n%s\n",
		 name, options[0] ? " " : "", options, devices);
	if (!ascii_cli_write_config(config) || !ascii_cli_capture_stderr())
		return false;

	argv[0] = (char*)bin;
	argv[1] = g_ascii_cli.config;
	argv[2] = NULL;
	return ascii_cli_start_with_args(argv);
}

static bool
ascii_cli_start_device(const char* devices)
{
	return ascii_cli_start_device_with_options("", devices, false);
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
send_ascii_req_fd(int fd, u8 addr, const u8* pdu, u32 pdu_len)
{
	u8 frame[MODBUS_ASCII_MAX_FRAME];
	u32 len = modbus_ascii_encode(frame, addr, pdu, pdu_len);

	return len != 0 && send_all_fd(fd, frame, len);
}

static bool
recv_ascii_frames_fd(int fd, u32 expected, u8* addrs,
		     u8 (*pdus)[MODBUS_MAX_PDU], u32* lens, u32 timeout_ms)
{
	u8 buf[MODBUS_ASCII_MAX_FRAME * 4];
	u32 got = 0;
	u32 frames = 0;
	u64 deadline = clock_now_ms() + timeout_ms;

	while (frames < expected)
	{
		u32 len = 0;
		modbus_ascii_scan_t scan = modbus_ascii_scan(buf, got, &len);

		if (scan == MODBUS_ASCII_SCAN_FRAME)
		{
			u8 decoded[MODBUS_MAX_PDU + 2];
			u8 addr = 0;
			const u8* pdu = NULL;
			u32 pdu_len = 0;

			if (!modbus_ascii_decode(buf, len, decoded, &addr, &pdu, &pdu_len))
				return false;
			addrs[frames] = addr;
			lens[frames] = pdu_len;
			memcpy(pdus[frames], pdu, pdu_len);
			frames++;
			memmove(buf, buf + len, got - len);
			got -= len;
			continue;
		}
		if (scan == MODBUS_ASCII_SCAN_DISCARD)
		{
			memmove(buf, buf + len, got - len);
			got -= len;
			continue;
		}
		if (got == sizeof(buf))
			return false;
		{
			u64 now = clock_now_ms();
			struct pollfd pfd = { .fd = fd, .events = POLLIN };
			int wait_ms;
			int rc;
			ssize_t n;

			if (now >= deadline)
				return false;
			wait_ms = (int)(deadline - now);
			rc = poll(&pfd, 1, wait_ms);
			if (rc < 0 && errno == EINTR)
				continue;
			if (rc <= 0)
				continue;
			if (pfd.revents & (POLLERR | POLLNVAL | POLLHUP))
				return false;
			if (!(pfd.revents & POLLIN))
				continue;
			n = read(fd, buf + got, sizeof(buf) - got);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				return false;
			got += (u32)n;
		}
	}
	return true;
}

static bool
ascii_cli_comm_error_count(u16* out)
{
	u8	req[] = { FC_DIAGNOSTICS, 0x00, 0x0C, 0x00, 0x00 };
	u8	addrs[1] = { 0 };
	u8	pdus[1][MODBUS_MAX_PDU] = { { 0 } };
	u32	lens[1] = { 0 };

	if (!send_ascii_req_fd(g_ascii_cli.master_fd, 1, req, sizeof(req)))
		return false;
	if (!recv_ascii_frames_fd(g_ascii_cli.master_fd, 1, addrs, pdus, lens,
				  ASCII_CLI_IO_TIMEOUT_MS))
		return false;
	if (addrs[0] != 1 || lens[0] != 5 || pdus[0][0] != FC_DIAGNOSTICS ||
	    pdus[0][1] != 0 || pdus[0][2] != 0x0C)
		return false;
	*out = get_be16(pdus[0] + 3);
	return true;
}

static bool
expect_silence_fd(int fd, u32 timeout_ms)
{
	u64 deadline = clock_now_ms() + timeout_ms;

	while (clock_now_ms() < deadline)
	{
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		u64 now = clock_now_ms();
		int wait_ms = (int)(deadline - now);
		int rc = poll(&pfd, 1, wait_ms);
		ssize_t n;

		if (rc < 0 && errno == EINTR)
			continue;
		if (rc <= 0)
			continue;
		if (pfd.revents & (POLLERR | POLLNVAL | POLLHUP))
			return false;
		if (!(pfd.revents & POLLIN))
			continue;
		n = read(fd, &(u8){ 0 }, 1);
		if (n < 0 && errno == EINTR)
			continue;
		return false;
	}
	return true;
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

TEST(AsciiCliServesOverPtyDevice)
{
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 addrs[1] = { 0 };
	u8 pdus[1][MODBUS_MAX_PDU] = { { 0 } };
	u32 lens[1] = { 0 };
	bool ok = false;

	ASSERT_EQ(ascii_cli_start_device("(device (unit-id 1) (holding 0 (static 4660)))\n"));

	ok = send_ascii_req_fd(g_ascii_cli.master_fd, 1, req, sizeof(req));
	ok = ok && recv_ascii_frames_fd(g_ascii_cli.master_fd, 1, addrs, pdus, lens,
					ASCII_CLI_IO_TIMEOUT_MS);
	ok = ok && addrs[0] == 1 && lens[0] == 4 && pdus[0][0] == FC_READ_HOLDING_REGISTERS &&
	     pdus[0][1] == 2 && get_be16(pdus[0] + 2) == 0x1234;

	ascii_cli_stop();
	ASSERT_EQ(ok);
}

TEST(AsciiCliDefaultLineFlagsAre8N1)
{
	struct termios tio;

	ASSERT_EQ(ascii_cli_start_device_with_options("",
						      "(device (unit-id 1) (holding 0))\n", true));
	ASSERT_EQ(tcgetattr(g_ascii_cli.master_fd, &tio) == 0);
	ASSERT_EQ((tio.c_cflag & CSIZE) == CS8);
	ASSERT_EQ((tio.c_cflag & (PARENB | PARODD | CSTOPB | CRTSCTS)) == 0);
	ASSERT_EQ((tio.c_cflag & (CLOCAL | CREAD)) == (CLOCAL | CREAD));
	ASSERT_EQ(cfgetispeed(&tio) == B19200 && cfgetospeed(&tio) == B19200);
	ascii_cli_stop();
}

TEST(AsciiCliSevenE1LineFlagsFromConfig)
{
	struct termios tio;
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 addrs[1] = { 0 };
	u8 pdus[1][MODBUS_MAX_PDU] = { { 0 } };
	u32 lens[1] = { 0 };
	bool ok = false;

	ASSERT_EQ(ascii_cli_start_device_with_options(
		"(data-bits 7) (parity even) (stop-bits 1)",
		"(device (unit-id 1) (holding 0 (static 4660)))\n", true));

	/* Linux PTYs force CS8 and strip PARENB, so tests/serial.c checks CS7 and
	 * PARENB directly; here verify the observable part of the 7E1 selection. */
	ok = tcgetattr(g_ascii_cli.master_fd, &tio) == 0;
	ok = ok && (tio.c_cflag & CSIZE) == CS8;
	ok = ok && (tio.c_cflag & (PARODD | CSTOPB | CRTSCTS)) == 0;
#ifdef CMSPAR
	ok = ok && (tio.c_cflag & CMSPAR) == 0;
#endif
	ok = ok && (tio.c_cflag & (CLOCAL | CREAD)) == (CLOCAL | CREAD);
	ok = ok && cfgetispeed(&tio) == B19200 && cfgetospeed(&tio) == B19200;
	ok = ok && send_ascii_req_fd(g_ascii_cli.master_fd, 1, req, sizeof(req));
	ok = ok && recv_ascii_frames_fd(g_ascii_cli.master_fd, 1, addrs, pdus, lens,
					ASCII_CLI_IO_TIMEOUT_MS);
	ok = ok && addrs[0] == 1 && lens[0] == 4 && get_be16(pdus[0] + 2) == 0x1234;

	ascii_cli_stop();
	ASSERT_EQ(ok);
}

TEST(AsciiCliChangeDelimiterRejectedAndCrlfFramingUnchanged)
{
	u8 delimiter_req[] = { FC_DIAGNOSTICS, 0x00, 0x03, 0x23, 0x00 };
	u8 register_req[]  = { FC_DIAGNOSTICS, 0x00, 0x02, 0x00, 0x00 };
	u8 read_req[]      = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 addrs[1] = { 0 };
	u8 pdus[1][MODBUS_MAX_PDU] = { { 0 } };
	u32 lens[1] = { 0 };
	bool ok = false;

	ASSERT_EQ(ascii_cli_start_device("(device (unit-id 1) (holding 0 (static 4660)))\n"));

	ok = send_ascii_req_fd(g_ascii_cli.master_fd, 1, delimiter_req, sizeof(delimiter_req));
	ok = ok && recv_ascii_frames_fd(g_ascii_cli.master_fd, 1, addrs, pdus, lens,
					ASCII_CLI_IO_TIMEOUT_MS);
	ok = ok && addrs[0] == 1 && lens[0] == 2 &&
	     pdus[0][0] == (FC_DIAGNOSTICS | 0x80) && pdus[0][1] == EXC_ILLEGAL_DATA_VALUE;

	ok = ok && send_ascii_req_fd(g_ascii_cli.master_fd, 1, register_req, sizeof(register_req));
	ok = ok && recv_ascii_frames_fd(g_ascii_cli.master_fd, 1, addrs, pdus, lens,
					ASCII_CLI_IO_TIMEOUT_MS);
	ok = ok && addrs[0] == 1 && lens[0] == 5 && get_be16(pdus[0] + 3) == 0;

	/* Framing is still CR LF: a normal request gets its normal answer. */
	ok = ok && send_ascii_req_fd(g_ascii_cli.master_fd, 1, read_req, sizeof(read_req));
	ok = ok && recv_ascii_frames_fd(g_ascii_cli.master_fd, 1, addrs, pdus, lens,
					ASCII_CLI_IO_TIMEOUT_MS);
	ok = ok && addrs[0] == 1 && lens[0] == 4 && get_be16(pdus[0] + 2) == 0x1234;

	ascii_cli_stop();
	ASSERT_EQ(ok);
}

TEST(AsciiCliSilentOnBroadcastAndUnknownUnit)
{
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 addrs[1] = { 0 };
	u8 pdus[1][MODBUS_MAX_PDU] = { { 0 } };
	u32 lens[1] = { 0 };
	u8 both[MODBUS_ASCII_MAX_FRAME * 2];
	u32 both_len = 0;
	bool ok = false;

	ASSERT_EQ(ascii_cli_start_device("(device (unit-id 1) (holding 0 (static 4660)))\n"));

	both_len = modbus_ascii_encode(both, 0, req, sizeof(req));
	both_len += modbus_ascii_encode(both + both_len, 2, req, sizeof(req));
	ok = send_all_fd(g_ascii_cli.master_fd, both, both_len);
	ok = ok && expect_silence_fd(g_ascii_cli.master_fd, 200);
	ok = ok && send_ascii_req_fd(g_ascii_cli.master_fd, 1, req, sizeof(req));
	ok = ok && recv_ascii_frames_fd(g_ascii_cli.master_fd, 1, addrs, pdus, lens,
					ASCII_CLI_IO_TIMEOUT_MS);
	ok = ok && addrs[0] == 1 && get_be16(pdus[0] + 2) == 0x1234;

	ascii_cli_stop();
	ASSERT_EQ(ok);
}

TEST(AsciiCliSilentOnBadLrc)
{
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	const char bad[] = ":010300000001FA\r\n";
	u8 addrs[1] = { 0 };
	u8 pdus[1][MODBUS_MAX_PDU] = { { 0 } };
	u32 lens[1] = { 0 };
	bool ok = false;

	ASSERT_EQ(ascii_cli_start_device("(device (unit-id 1) (holding 0 (static 4660)))\n"));

	ok = send_all_fd(g_ascii_cli.master_fd, (const u8*)bad, sizeof(bad) - 1);
	ok = ok && expect_silence_fd(g_ascii_cli.master_fd, 200);
	ok = ok && send_ascii_req_fd(g_ascii_cli.master_fd, 1, req, sizeof(req));
	ok = ok && recv_ascii_frames_fd(g_ascii_cli.master_fd, 1, addrs, pdus, lens,
					ASCII_CLI_IO_TIMEOUT_MS);
	ok = ok && addrs[0] == 1 && get_be16(pdus[0] + 2) == 0x1234;

	ascii_cli_stop();
	ASSERT_EQ(ok);
}

TEST(AsciiCliCommErrorsIgnoreInterFrameWhitespace)
{
	const char	bad_lrc[] = ":010300000001FA\r\n";
	const char	gap[] = "\r\n \r\n";
	const char	bad_framing[] = ":0103\n";
	u8		bad_addr[] = { FC_READ_HOLDING_REGISTERS, 0x12, 0x34, 0x00, 0x01 };
	u8		addrs[1] = { 0 };
	u8		pdus[1][MODBUS_MAX_PDU] = { { 0 } };
	u32		lens[1] = { 0 };
	u16		count = 0;
	bool		ok = false;

	ASSERT_EQ(ascii_cli_start_device("(device (unit-id 1) (holding 0 (static 4660)))\n"));

	ok = send_all_fd(g_ascii_cli.master_fd, (const u8*)bad_lrc, sizeof(bad_lrc) - 1);
	ok = ok && expect_silence_fd(g_ascii_cli.master_fd, 100);
	if (ok) ok = ascii_cli_comm_error_count(&count) && count == 1;

	/* A whitespace-only gap between frames is not a malformed frame. */
	if (ok) ok = send_all_fd(g_ascii_cli.master_fd, (const u8*)gap, sizeof(gap) - 1);
	if (ok) ok = expect_silence_fd(g_ascii_cli.master_fd, 100);
	if (ok) ok = ascii_cli_comm_error_count(&count) && count == 1;

	/* A frame discarded by the scanner (lone LF) still counts as one error. */
	if (ok) ok = send_all_fd(g_ascii_cli.master_fd, (const u8*)bad_framing,
				 sizeof(bad_framing) - 1);
	if (ok) ok = expect_silence_fd(g_ascii_cli.master_fd, 100);
	if (ok) ok = ascii_cli_comm_error_count(&count) && count == 2;

	/* A decoded request returning an exception is not a communication error. */
	if (ok) ok = send_ascii_req_fd(g_ascii_cli.master_fd, 1, bad_addr, sizeof(bad_addr));
	if (ok) ok = recv_ascii_frames_fd(g_ascii_cli.master_fd, 1, addrs, pdus, lens,
					  ASCII_CLI_IO_TIMEOUT_MS);
	if (ok)
		ok = addrs[0] == 1 && lens[0] == 2 &&
		     (pdus[0][0] & 0x80) != 0 && pdus[0][1] == EXC_ILLEGAL_DATA_ADDRESS;
	if (ok) ok = ascii_cli_comm_error_count(&count) && count == 2;

	ascii_cli_stop();
	ASSERT_EQ(ok);
}

TEST(AsciiCliProcessesMultipleFramesInOneWrite)
{
	u8 req0[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8 req1[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x01, 0x00, 0x01 };
	u8 addrs[2] = { 0 };
	u8 pdus[2][MODBUS_MAX_PDU] = { { 0 } };
	u32 lens[2] = { 0 };
	u8 both[MODBUS_ASCII_MAX_FRAME * 2];
	u32 both_len = 0;
	bool ok = false;

	ASSERT_EQ(ascii_cli_start_device(
		"(device (unit-id 1) (holding 0 (static 4660)) (holding 1 (static 1234)))\n"));

	both_len = modbus_ascii_encode(both, 1, req0, sizeof(req0));
	both_len += modbus_ascii_encode(both + both_len, 1, req1, sizeof(req1));
	ok = send_all_fd(g_ascii_cli.master_fd, both, both_len);
	ok = ok && recv_ascii_frames_fd(g_ascii_cli.master_fd, 2, addrs, pdus, lens,
					ASCII_CLI_IO_TIMEOUT_MS);
	ok = ok && addrs[0] == 1 && lens[0] == 4 && get_be16(pdus[0] + 2) == 0x1234;
	ok = ok && addrs[1] == 1 && lens[1] == 4 && get_be16(pdus[1] + 2) == 0x04D2;

	ascii_cli_stop();
	ASSERT_EQ(ok);
}

TEST(AsciiCliFaultDropSuppressesResponse)
{
	u8 req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	bool ok = false;

	ASSERT_EQ(ascii_cli_start_device(
		"(device (unit-id 1) (holding 0 (static 7)) (fault (kind drop) (percent 100)))\n"));

	ok = send_ascii_req_fd(g_ascii_cli.master_fd, 1, req, sizeof(req));
	ok = ok && expect_silence_fd(g_ascii_cli.master_fd, 250);

	ascii_cli_stop();
	ASSERT_EQ(ok);
}

static bool
ascii_cli_run_check(const char* config_text, char* const argv[],
		    int expected_status, const char* stderr_needle)
{
	int status = 0;

	if (!ascii_cli_write_config(config_text) || !ascii_cli_capture_stderr())
		return false;
	if (!ascii_cli_start_with_args(argv))
		return false;
	if (!wait_for_process_exit(g_ascii_cli.pid, &status, 2000))
		return false;
	g_ascii_cli.pid = -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != expected_status)
		return false;
	if (stderr_needle && !ascii_cli_file_contains(g_ascii_cli.stderr_path, stderr_needle))
		return false;
	ascii_cli_stop();
	return true;
}

TEST(AsciiCliConfigModeValidatesUnitIdsAndRejectsRemovedFlags)
{
	const char* bin = getenv("MODBUS_SIM_BIN");
	const char* ascii_valid = "(server (mode ascii-pty))\n"
				  "(device (unit-id 1) (holding 0 (static 1)))\n";
	const char* ascii_reserved = "(server (mode ascii-pty))\n"
				     "(device (unit-id 248) (holding 0))\n";
	const char* ascii_7e1 = "(server (mode ascii-device) (path \"/dev/ttyS0\")"
				" (data-bits 7) (parity even) (stop-bits 1))\n"
				"(device (unit-id 1) (holding 0 (static 1)))\n";
	const char* ascii_broadcast = "(server (mode ascii-pty) (broadcast-execute on))\n"
				      "(device (unit-id 1) (holding 0 (static 1)))\n";
	const char* valid = "(device (unit-id 1) (holding 0 (static 1)))\n";
	bool ok = true;

	if (!bin)
		bin = "./modbus-sim";

	{
		char* argv[] = { (char*)bin, "--check", g_ascii_cli.config, NULL };

		ok = ok && ascii_cli_run_check(ascii_reserved, argv, 1,
					       "not valid for RTU/ASCII");
	}
	{
		char* argv[] = { (char*)bin, "--check", g_ascii_cli.config, NULL };

		ok = ok && ascii_cli_run_check(ascii_valid, argv, 0, NULL);
	}
	{
		char* argv[] = { (char*)bin, "--check", g_ascii_cli.config, NULL };

		ok = ok && ascii_cli_run_check(ascii_7e1, argv, 0, NULL);
	}
	{
		char* argv[] = { (char*)bin, "--check", g_ascii_cli.config, NULL };

		ok = ok && ascii_cli_run_check(ascii_broadcast, argv, 1,
					       "ASCII never executes unit-0 broadcasts");
	}
	{
		char* argv[] = { (char*)bin, "--ascii-pty", "--port", "1502",
				 g_ascii_cli.config, NULL };

		ok = ok && ascii_cli_run_check(valid, argv, 2, "unknown option");
	}
	ASSERT_EQ(ok);
}
