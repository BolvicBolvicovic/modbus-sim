#define _GNU_SOURCE
#include "tests/tests_utils/test_support.h"
#include "tests/tests_utils/test_os.h"
#include "modbus/ascii.h"
#include "modbus/rtu.h"
#include "os/os_time.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Process and temporary-file helpers. */

bool
ts_verbose(void)
{
	const char* v = getenv("MOGBUS_TEST_VERBOSE");

	return v && v[0] && strcmp(v, "0") != 0;
}

#ifndef _WIN32  /* Windows: test_support_win32.c */
bool
ts_temp_file(char* path, size_t cap, const char* tmpl)
{
	int fd;

	snprintf(path, cap, "%s", tmpl);
	fd = mkstemp(path);
	if (fd < 0)
	{
		path[0] = '\0';
		return false;
	}
	close(fd);
	return true;
}

bool
ts_write_config(ts_server_t* s, const char* tmpl, const char* text)
{
	FILE* file;
	int   fd;

	snprintf(s->config, sizeof(s->config), "%s", tmpl);
	fd = mkstemp(s->config);
	if (fd < 0)
	{
		s->config[0] = '\0';
		return false;
	}
	file = fdopen(fd, "w");
	if (!file)
	{
		close(fd);
		unlink(s->config);
		s->config[0] = '\0';
		return false;
	}
	fputs(text, file);
	fclose(file);
	return true;
}
#endif

bool
ts_file_contains(const char* path, const char* needle)
{
	FILE* file;
	char  line[512];
	bool  found = false;

	if (!path[0])
		return false;
	file = fopen(path, "r");
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

void
ts_dump_stderr(const ts_server_t* s)
{
	FILE* file;
	char  line[512];

	if (!s->stderr_path[0])
		return;
	file = fopen(s->stderr_path, "r");
	if (!file)
		return;
	fprintf(stderr, "--- %s stderr (%s) ---\n", s->label, s->stderr_path);
	while (fgets(line, sizeof(line), file))
		fputs(line, stderr);
	fprintf(stderr, "--- end %s stderr ---\n", s->label);
	fclose(file);
}

void
ts_server_init(ts_server_t* s, const char* label)
{
	memset(s, 0, sizeof(*s));
	s->pid       = -1;
	s->master_fd = -1;
	s->probe_family = AF_INET;
	s->flags     = TS_CAPTURE_STDERR | TS_CLEAN_EXIT;
	snprintf(s->label, sizeof(s->label), "%s", label);
	snprintf(s->ready_needle, sizeof(s->ready_needle), "listening on");
}

#ifndef _WIN32  /* Windows: test_support_win32.c */
/* waitid with WNOWAIT notices an early exit without reaping it. */
bool
ts_process_done(pid_t pid)
{
	siginfo_t info = { 0 };

	return waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
	       info.si_pid == pid;
}

bool
ts_wait_pid(pid_t pid, int* status, u32 timeout_ms)
{
	u64 deadline = os_now_ms() + timeout_ms;

	for (;;)
	{
		pid_t done = waitpid(pid, status, WNOHANG);

		if (done == pid)
			return true;
		if (done < 0 && errno == ECHILD)
			return false;
		if (os_now_ms() >= deadline)
		{
			kill(pid, SIGKILL);
			return waitpid(pid, status, 0) == pid;
		}
		usleep(TS_STOP_POLL_US);
	}
}

/* Verbose runs keep stderr on the console, so use a connection probe instead.
 * Network fixtures set probe_port; CLI fixtures keep the historical short wait. */
static bool
server_wait_ready_verbose(ts_server_t* s, pid_t pid)
{
	if (!s->probe_port)
	{
		usleep(TS_START_FALLBACK_MS * 1000);
		return kill(pid, 0) == 0;
	}
	for (u32 i = 0; i < 300; i++)
	{
		int fd = ts_connect_loopback(s->probe_port, s->probe_family, 50);

		if (fd >= 0)
		{
			close(fd);
			usleep(50000); /* let the server reap the probe connection */
			return true;
		}
		usleep(10000);
	}
	return false;
}

bool
ts_wait_startup(ts_server_t* s, pid_t pid)
{
	u64 deadline;

	if (!s->stderr_path[0])
		return server_wait_ready_verbose(s, pid);

	deadline = os_now_ms() + TS_START_TIMEOUT_MS;
	for (;;)
	{
		if (ts_file_contains(s->stderr_path, s->ready_needle))
			return kill(pid, 0) == 0;
		if (ts_process_done(pid))
		{
			if (s->flags & TS_EXIT_IS_READY)
				return kill(pid, 0) == 0;
			fprintf(stderr, "%s: pid %d exited before readiness\n", s->label, (int)pid);
			ts_dump_stderr(s);
			return false;
		}
		if (os_now_ms() >= deadline)
		{
			fprintf(stderr, "%s: pid %d never became ready\n", s->label, (int)pid);
			ts_dump_stderr(s);
			return false;
		}
		usleep(TS_READY_POLL_MS * 1000);
	}
}

bool
ts_server_start(ts_server_t* s, char* const argv[])
{
	bool capture_err = (s->flags & TS_CAPTURE_STDERR) && !ts_verbose();
	bool capture_out = (s->flags & TS_CAPTURE_STDOUT) && !ts_verbose();

	/* Callers that need the log path before starting (CLI checks) create it themselves. */
	if (capture_err && !s->stderr_path[0] &&
	    !ts_temp_file(s->stderr_path, sizeof(s->stderr_path), "/tmp/mogbus-stderr-XXXXXX"))
		return false;
	if (capture_out && !s->stdout_path[0] &&
	    !ts_temp_file(s->stdout_path, sizeof(s->stdout_path), "/tmp/mogbus-stdout-XXXXXX"))
		goto fail;

	s->pid = fork();
	if (s->pid == 0)
	{
		if ((s->flags & TS_NULL_STDIN) && !freopen("/dev/null", "r", stdin))
			_exit(127);
		if (capture_out)
		{
			if (!freopen(s->stdout_path, "w", stdout))
				_exit(127);
			setvbuf(stdout, NULL, _IONBF, 0);
		}
		else if ((s->flags & TS_NULL_STDOUT) && !freopen("/dev/null", "w", stdout))
			_exit(127);
		if (capture_err && !freopen(s->stderr_path, "w", stderr))
			_exit(127);
		if (capture_err)
			setvbuf(stderr, NULL, _IONBF, 0);
		if (s->nofile > 0)
		{
			struct rlimit lim = { .rlim_cur = s->nofile, .rlim_max = s->nofile };

			if (setrlimit(RLIMIT_NOFILE, &lim) < 0)
				_exit(126);
		}
		if (s->flags & TS_DEATH_SIGNAL)
		{
			prctl(PR_SET_PDEATHSIG, SIGTERM);
			if (getppid() == 1)
				_exit(0);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	if (s->pid <= 0)
		goto fail;

	if (ts_wait_startup(s, s->pid))
		return true;

	/* Startup failed: clean up the child and temp files before returning. */
	(void)ts_server_stop(s);
	return false;

fail:
	if (s->config[0])
		unlink(s->config);
	if (s->stderr_path[0])
		unlink(s->stderr_path);
	if (s->stdout_path[0])
		unlink(s->stdout_path);
	s->config[0] = '\0';
	s->stderr_path[0] = '\0';
	s->stdout_path[0] = '\0';
	s->nofile = 0;
	return false;
}

bool
ts_server_stop(ts_server_t* s)
{
	int  status = 0;
	bool reaped = false;
	bool ok     = true;
	pid_t pid   = s->pid;

	s->pid = -1;
	if (s->master_fd >= 0)
	{
		close(s->master_fd);
		s->master_fd = -1;
	}
	if (pid > 0)
	{
		kill(pid, SIGTERM);
		for (u32 i = 0; i < TS_STOP_ATTEMPTS; i++)
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
			usleep(TS_STOP_POLL_US);
		}
		if (!reaped && ok && waitpid(pid, &status, WNOHANG) != pid)
		{
			kill(pid, SIGKILL);
			reaped = waitpid(pid, &status, 0) == pid;
		}
		if (reaped)
		{
			if (s->flags & TS_CLEAN_EXIT)
				ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
			else
				ok = WIFEXITED(status) ? WEXITSTATUS(status) == 0 : WIFSIGNALED(status);
		}
	}
	if (!ok)
	{
		ts_dump_stderr(s);
		fprintf(stderr, "%s: pid %d exited=%d status=%d signal=%d\n", s->label, (int)pid,
			WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
			WIFSIGNALED(status) ? WTERMSIG(status) : -1);
	}

	if (s->config[0])
		unlink(s->config);
	if (s->stderr_path[0])
		unlink(s->stderr_path);
	if (s->stdout_path[0])
		unlink(s->stdout_path);
	s->config[0] = '\0';
	s->stderr_path[0] = '\0';
	s->stdout_path[0] = '\0';
	s->nofile = 0;
	return ok;
}

void
ts_server_cleanup(ts_server_t* s)
{
	pid_t pid = s->pid;

	s->pid = -1;
	if (s->master_fd >= 0)
	{
		close(s->master_fd);
		s->master_fd = -1;
	}
	if (pid > 0)
	{
		kill(pid, SIGTERM);
		(void)ts_wait_pid(pid, NULL, TS_STOP_ATTEMPTS * TS_STOP_POLL_US / 1000);
	}

	if (s->config[0])
		unlink(s->config);
	if (s->stderr_path[0])
		unlink(s->stderr_path);
	if (s->stdout_path[0])
		unlink(s->stdout_path);
	s->config[0] = '\0';
	s->stderr_path[0] = '\0';
	s->stdout_path[0] = '\0';
	s->nofile = 0;
}
#endif

#ifndef _WIN32
int
ts_exec(char* const argv[], const char* input, char* out, size_t cap)
{
	int   in_pipe[2];
	int   out_pipe[2];
	pid_t pid;
	size_t used = 0;
	int   status = 0;

	out[0] = '\0';
	if (pipe(in_pipe) < 0)
		return -1;
	if (pipe(out_pipe) < 0)
	{
		close(in_pipe[0]);
		close(in_pipe[1]);
		return -1;
	}
	pid = fork();
	if (pid == 0)
	{
		dup2(in_pipe[0], STDIN_FILENO);
		dup2(out_pipe[1], STDOUT_FILENO);
		dup2(out_pipe[1], STDERR_FILENO);
		close(in_pipe[0]);
		close(in_pipe[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		execv(argv[0], argv);
		_exit(127);
	}
	close(in_pipe[0]);
	close(out_pipe[1]);
	if (pid < 0)
	{
		close(in_pipe[1]);
		close(out_pipe[0]);
		return -1;
	}
	if (input && input[0] && write(in_pipe[1], input, strlen(input)) < 0)
		status = -1;
	close(in_pipe[1]);
	for (;;)
	{
		char    sink[512];
		ssize_t n = read(out_pipe[0], used + 1 < cap ? out + used : sink, used + 1 < cap ? cap - 1 - used : sizeof(sink));

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (used + 1 < cap)
			used += (size_t)n;
	}
	close(out_pipe[0]);
	out[used] = '\0';
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

int
ts_split_args(char* line, char* argv[], int max)
{
	int n = 0;

	while (*line && n < max - 1)
	{
		char* w;

		while (*line == ' ')
			line++;
		if (!*line)
			break;
		argv[n++] = w = line;
		for (bool quoted = false; *line && (quoted || *line != ' '); line++)
		{
			if (*line == '\'')
				quoted = !quoted;
			else
				*w++ = *line;
		}
		if (*line)
			line++;
		*w = '\0';
	}
	argv[n] = NULL;
	return n;
}

/* fd and socket I/O. */

bool
ts_send_fd(int fd, const u8* buf, u32 len)
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

bool
ts_send_socket(int fd, const u8* buf, u32 len)
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

void
ts_set_recv_timeout(int fd, u32 timeout_ms)
{
#ifdef _WIN32
	DWORD ms = timeout_ms;	/* Winsock takes milliseconds, not a timeval */

	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &ms, sizeof(ms));
#else
	struct timeval tv = { .tv_sec = (time_t)(timeout_ms / 1000),
			      .tv_usec = (suseconds_t)(timeout_ms % 1000) * 1000 };

	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

bool
ts_recv_exact(int fd, u8* buf, u32 len, u32 timeout_ms)
{
	ts_set_recv_timeout(fd, timeout_ms);
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

bool
ts_no_data(int fd, u32 timeout_ms)
{
	u64 deadline = os_now_ms() + timeout_ms;

	while (os_now_ms() < deadline)
	{
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		u64   now     = os_now_ms();
		int   wait_ms = (int)(deadline - now);
		int   rc      = poll(&pfd, 1, wait_ms);

		if (rc < 0 && errno == EINTR)
			continue;
		if (rc <= 0)
			continue;
		if (pfd.revents & POLLIN)
		{
			u8    byte = 0;
			ssize_t got = read(fd, &byte, 1);

			if (got < 0 && errno == EINTR)
				continue;
			return false;
		}
		if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
			return false;
	}
	return true;
}

bool
ts_wait_eof(int fd, u32 timeout_ms)
{
	u8 byte = 0;

	ts_set_recv_timeout(fd, timeout_ms);
	for (;;)
	{
		ssize_t got = recv(fd, &byte, 1, 0);

		if (got == 0)
			return true;
		if (got < 0 && errno == EINTR)
			continue;
		return false;
	}
}

/* Loopback helpers. */

u16
ts_free_port(void)
{
	/* Tests run in parallel. A port from bind(0) is an ephemeral port, which any
	 * test's outgoing connection may take before the server binds it, so pick a
	 * random port below the usual ephemeral range (32768+) and check that it is
	 * free for both TCP and UDP. */
	static u32 state;

	if (state == 0)
		state = (u32)getpid() * 2654435761u ^ (u32)os_now_ms();
	for (u32 attempt = 0; attempt < 200; attempt++)
	{
		struct sockaddr_in addr = { 0 };
		bool               free_port = true;
		u16                port;

		state = state * 1103515245u + 12345u;
		port  = (u16)(10000 + (state >> 8) % 22000);
		addr.sin_family      = AF_INET;
		addr.sin_port        = htons(port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		for (int type = 0; type < 2 && free_port; type++)
		{
			int fd = (int)socket(AF_INET, type == 0 ? SOCK_STREAM : SOCK_DGRAM, 0);

			free_port = fd >= 0 && bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0;
			if (fd >= 0)
				close(fd);
		}
		if (free_port)
			return port;
	}
	return 0;
}

bool
ts_ipv6_loopback_available(void)
{
	struct sockaddr_in6 addr = { 0 };
	int                 fd   = (int)socket(AF_INET6, SOCK_STREAM, 0);

	if (fd < 0)
		return false;
	addr.sin6_family = AF_INET6;
	addr.sin6_port   = 0;
	if (inet_pton(AF_INET6, "::1", &addr.sin6_addr) != 1 ||
	    bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		close(fd);
		return false;
	}
	close(fd);
	return true;
}

static int
connect_loopback(u16 port, int family, int type, u32 timeout_ms)
{
	int fd = (int)socket(family, type, 0);

	if (fd < 0)
		return -1;
	ts_set_recv_timeout(fd, timeout_ms);
#ifdef _WIN32
	{
		DWORD ms = timeout_ms;

		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &ms, sizeof(ms));
	}
#else
	{
		struct timeval tv = { .tv_sec = (time_t)(timeout_ms / 1000),
				      .tv_usec = (suseconds_t)(timeout_ms % 1000) * 1000 };

		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}
#endif
	if (family == AF_INET)
	{
		struct sockaddr_in a = { 0 };

		a.sin_family = AF_INET;
		a.sin_port   = htons(port);
		inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
		if (connect(fd, (struct sockaddr*)&a, sizeof(a)) == 0)
			return fd;
	}
	else
	{
		struct sockaddr_in6 a = { 0 };

		a.sin6_family = AF_INET6;
		a.sin6_port   = htons(port);
		inet_pton(AF_INET6, "::1", &a.sin6_addr);
		if (connect(fd, (struct sockaddr*)&a, sizeof(a)) == 0)
			return fd;
	}
	close(fd);
	return -1;
}

int
ts_connect_loopback(u16 port, int family, u32 timeout_ms)
{
	return connect_loopback(port, family, SOCK_STREAM, timeout_ms);
}

int
ts_udp_loopback(u16 port, int family, u32 timeout_ms)
{
	return connect_loopback(port, family, SOCK_DGRAM, timeout_ms);
}

s32
ts_udp_recv(int fd, u8* buf, u32 cap, u32 timeout_ms)
{
	ssize_t got;

	ts_set_recv_timeout(fd, timeout_ms);
	do
		got = recv(fd, buf, cap, 0);
	while (got < 0 && errno == EINTR);
	return got < 0 ? -1 : (s32)got;
}

bool
ts_udp_mbap_recv(int fd, u8 unit, u16 tid, u8* pdu, u32* pdu_len, u32 timeout_ms)
{
	u8  raw[MBAP_MAX_FRAME];
	s32 got = ts_udp_recv(fd, raw, sizeof(raw), timeout_ms);

	if (got < MBAP_HEADER_SIZE + 1 ||
	    !ts_mbap_reply_header(raw, unit, tid, pdu_len) ||
	    MBAP_HEADER_SIZE + *pdu_len != (u32)got)
		return false;
	memcpy(pdu, raw + MBAP_HEADER_SIZE, *pdu_len);
	return true;
}

/* Wire-frame helpers. */

u16
ts_next_tid(void)
{
	static u16 tid = 0;

	return ++tid;
}

u32
ts_mbap_encode(u8* frame, u8 unit, u16 tid, const u8* pdu, u32 pdu_len)
{
	mbap_header_t header = { .transaction = tid, .length = (u16)(pdu_len + 1), .unit = unit };

	mbap_encode(frame, &header);
	memcpy(frame + MBAP_HEADER_SIZE, pdu, pdu_len);
	return MBAP_HEADER_SIZE + pdu_len;
}

bool
ts_mbap_reply_header(const u8* raw, u8 unit, u16 tid, u32* pdu_len)
{
	mbap_header_t header = { 0 };

	mbap_decode(raw, &header);
	if (!mbap_is_valid(&header) || header.transaction != tid || header.unit != unit)
		return false;
	*pdu_len = header.length - 1;
	return true;
}

bool
ts_mbap_send(int fd, u8 unit, u16 tid, const u8* pdu, u32 pdu_len)
{
	u8 frame[MBAP_MAX_FRAME];

	ts_mbap_encode(frame, unit, tid, pdu, pdu_len);
	return ts_send_socket(fd, frame, MBAP_HEADER_SIZE + pdu_len);
}

bool
ts_mbap_recv(int fd, u8 unit, u16 tid, u8* pdu, u32* pdu_len, u32 timeout_ms)
{
	u8 raw[MBAP_HEADER_SIZE];

	if (!ts_recv_exact(fd, raw, sizeof(raw), timeout_ms))
		return false;
	if (!ts_mbap_reply_header(raw, unit, tid, pdu_len))
		return false;
	return ts_recv_exact(fd, pdu, *pdu_len, timeout_ms);
}

bool
ts_rtu_send(int fd, u8 addr, const u8* pdu, u32 pdu_len, bool on_socket)
{
	u8  frame[MODBUS_RTU_MAX_FRAME];
	u32 len = modbus_rtu_encode(frame, addr, pdu, pdu_len);

	return len > 0 && (on_socket ? ts_send_socket(fd, frame, len) : ts_send_fd(fd, frame, len));
}

bool
ts_rtu_recv(int fd, u8* addr, u8* pdu, u32* pdu_len, u32 timeout_ms, bool on_socket)
{
	u8  frame[MODBUS_RTU_MAX_FRAME];
	u32 got = 0;

	if (!on_socket)
	{
		u64 deadline = os_now_ms() + timeout_ms;

		while (got < sizeof(frame))
		{
			struct pollfd pfd = { .fd = fd, .events = POLLIN };
			u64   now = os_now_ms();
			int   wait_ms;
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
			n = read(fd, frame + got, sizeof(frame) - got);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				return false;
			got += (u32)n;
			if (got >= 4 && modbus_rtu_decode(frame, got, &out_addr, &out_pdu, &out_len))
			{
				*addr = out_addr;
				*pdu_len = out_len;
				memcpy(pdu, out_pdu, out_len);
				return true;
			}
		}
		return false;
	}

	while (got < sizeof(frame))
	{
		const u8* out_pdu = NULL;
		u32 out_len = 0;
		u8 out_addr = 0;

		if (!ts_recv_exact(fd, frame + got, 1, timeout_ms))
			return false;
		got++;
		if (got >= 4 && modbus_rtu_decode(frame, got, &out_addr, &out_pdu, &out_len))
		{
			*addr = out_addr;
			*pdu_len = out_len;
			memcpy(pdu, out_pdu, out_len);
			return true;
		}
	}
	return false;
}

bool
ts_ascii_send(int fd, u8 addr, const u8* pdu, u32 pdu_len, bool on_socket)
{
	u8  frame[MODBUS_ASCII_MAX_FRAME];
	u32 len = modbus_ascii_encode(frame, addr, pdu, pdu_len);

	return len > 0 && (on_socket ? ts_send_socket(fd, frame, len) : ts_send_fd(fd, frame, len));
}

bool
ts_ascii_recv_frame(int fd, u8* frame, u32* len, u32 timeout_ms, bool on_socket)
{
	u32 got = 0;

	if (!on_socket)
	{
		u64 deadline = os_now_ms() + timeout_ms;

		while (got < MODBUS_ASCII_MAX_FRAME)
		{
			modbus_ascii_scan_t scan;
			u32 scan_len = 0;
			ssize_t n;
			struct pollfd pfd = { .fd = fd, .events = POLLIN };
			u64 now = os_now_ms();
			int wait_ms;

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
			n = read(fd, frame + got, 1);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				return false;
			got++;
			scan = modbus_ascii_scan(frame, got, &scan_len);
			if (scan == MODBUS_ASCII_SCAN_FRAME)
			{
				*len = scan_len;
				return true;
			}
			if (scan == MODBUS_ASCII_SCAN_DISCARD)
				return false;
		}
		return false;
	}

	while (got < MODBUS_ASCII_MAX_FRAME)
	{
		modbus_ascii_scan_t scan;
		u32 scan_len = 0;

		if (!ts_recv_exact(fd, frame + got, 1, timeout_ms))
			return false;
		got++;
		scan = modbus_ascii_scan(frame, got, &scan_len);
		if (scan == MODBUS_ASCII_SCAN_FRAME)
		{
			*len = scan_len;
			return true;
		}
		if (scan == MODBUS_ASCII_SCAN_DISCARD)
			return false;
	}
	return false;
}

bool
ts_ascii_recv_response(int fd, u8* addr, u8* pdu, u32* pdu_len, u32 timeout_ms, bool on_socket)
{
	u8 frame[MODBUS_ASCII_MAX_FRAME];
	u8 decoded[MODBUS_MAX_PDU + 2];
	const u8* body = NULL;
	u32 len = 0;

	if (!ts_ascii_recv_frame(fd, frame, &len, timeout_ms, on_socket))
		return false;
	if (!modbus_ascii_decode(frame, len, decoded, addr, &body, pdu_len))
		return false;
	memcpy(pdu, body, *pdu_len);
	return true;
}

/* Buffered multi-frame receive for byte-stream fixtures that may coalesce replies. */
bool
ts_ascii_recv_frames(int fd, u32 expected, u8* addrs, u8 (*pdus)[MODBUS_MAX_PDU],
		     u32* lens, u32 timeout_ms)
{
	u8  buf[MODBUS_ASCII_MAX_FRAME * 4];
	u32 got = 0;
	u32 frames = 0;
	u64 deadline = os_now_ms() + timeout_ms;

	while (frames < expected)
	{
		modbus_ascii_scan_t scan;
		u32 len = 0;

		scan = modbus_ascii_scan(buf, got, &len);
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
			u64 now = os_now_ms();
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
