/** @file
 *  @brief Windows half of test_support: spawning the binary with CreateProcess, stopping it, temp
 *         files and the descriptor shims of test_os.h.
 *
 * There is no fork and no signal here. A server is stopped through the named event
 * `Local\mogbus-stop-<pid>` that `os_console_on_stop()` creates (the same request as Ctrl-C, but
 * with no console needed); a server that ignores it is terminated. Children run in a job object
 * that kills them when the test process dies, and only the three standard handles are inherited,
 * so a child never keeps a test's sockets alive.
 */
#ifdef _WIN32
#include "tests/tests_utils/test_support.h"
#include "os/os_time.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Exit status reported for a process the harness had to terminate. */
enum
{
	TS_KILLED_STATUS = 0x40000001
};

static HANDLE
as_handle(pid_t pid)
{
	return (HANDLE)(intptr_t)pid;
}

static pid_t
from_handle(HANDLE h)
{
	return (pid_t)(intptr_t)h;
}

/* Winsock must be up before a test's first socket() call. */
__attribute__((constructor)) static void
ts_winsock_init(void)
{
	WSADATA wsa;

	WSAStartup(MAKEWORD(2, 2), &wsa);
}

/* Descriptor shims (see test_os.h): a socket or a C runtime descriptor. */

static bool
is_socket(int fd)
{
	int type = 0;
	int len  = (int)sizeof(type);

	return getsockopt((SOCKET)fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0;
}

int
ts_close(int fd)
{
	return is_socket(fd) ? closesocket((SOCKET)fd) : _close(fd);
}

int
ts_read(int fd, void* buf, unsigned len)
{
	if (is_socket(fd))
	{
		int got = recv((SOCKET)fd, buf, (int)len, 0);

		return got;
	}
	return _read(fd, buf, len);
}

int
ts_write(int fd, const void* buf, unsigned len)
{
	if (is_socket(fd))
		return send((SOCKET)fd, buf, (int)len, 0);
	return _write(fd, buf, len);
}

/* A connected pair of loopback TCP sockets, standing in for socketpair(). */
int
ts_socketpair(int domain, int type, int protocol, int sv[2])
{
	struct sockaddr_in addr = { 0 };
	int		   len  = (int)sizeof(addr);
	SOCKET		   listener, client, server;

	(void)domain;
	(void)type;
	(void)protocol;
	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener == INVALID_SOCKET)
		return -1;
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	client = socket(AF_INET, SOCK_STREAM, 0);
	if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
	    listen(listener, 1) != 0 ||
	    getsockname(listener, (struct sockaddr*)&addr, &len) != 0 ||
	    client == INVALID_SOCKET ||
	    connect(client, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
	    (server = accept(listener, NULL, NULL)) == INVALID_SOCKET)
	{
		if (client != INVALID_SOCKET)
			closesocket(client);
		closesocket(listener);
		return -1;
	}
	closesocket(listener);
	sv[0] = (int)client;
	sv[1] = (int)server;
	return 0;
}

/* Temporary files. */

static bool
make_temp(char* path, size_t cap, const char* tmpl)
{
	static LONG counter;
	char        dir[MAX_PATH];
	char        base[48];
	const char* slash = strrchr(tmpl, '/');
	size_t      n;
	HANDLE      h;

	slash = slash ? slash + 1 : tmpl;
	snprintf(base, sizeof(base), "%s", slash);
	n = strlen(base);
	while (n > 0 && base[n - 1] == 'X')
		base[--n] = '\0';
	if (GetTempPathA(sizeof(dir), dir) == 0)
		return false;
	for (char* c = dir; *c; c++)
		if (*c == '\\')
			*c = '/';
	if (strlen(dir) + strlen(base) + 32 > cap)
		dir[0] = '\0';	/* a long temp path does not fit the callers' buffers: use the working directory */
	for (u32 attempt = 0; attempt < 100; attempt++)
	{
		snprintf(path, cap, "%s%s%lu-%lu-%lu", dir, base, (unsigned long)GetCurrentProcessId(),
			 (unsigned long)InterlockedIncrement(&counter), (unsigned long)GetTickCount());
		h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
				CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		if (h != INVALID_HANDLE_VALUE)
		{
			CloseHandle(h);
			return true;
		}
	}
	path[0] = '\0';
	return false;
}

/* mkstemp() for the tests: "/tmp/NAME-XXXXXX" becomes "NAME-<unique>" in the working directory
 * (the result must fit the template, which a real temp directory path would not). */
int
ts_mkstemp(char* tmpl)
{
	static LONG counter;
	char*       name = strrchr(tmpl, '/');
	size_t      len;
	char*       x;

	if (name)
		memmove(tmpl, name + 1, strlen(name + 1) + 1);
	len = strlen(tmpl);
	x   = tmpl + len;
	while (x > tmpl && x[-1] == 'X')
		x--;
	if (tmpl + len - x < 6)
		return -1;
	for (u32 attempt = 0; attempt < 100; attempt++)
	{
		unsigned long n = ((unsigned long)GetCurrentProcessId() << 12) ^ (unsigned long)InterlockedIncrement(&counter) ^
				  (unsigned long)GetTickCount() * 31;
		int           fd;

		for (char* c = tmpl + len; c > x;)
		{
			*--c = "abcdefghijklmnopqrstuvwxyz012345"[n & 31];
			n >>= 5;
		}
		fd = _open(tmpl, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
		if (fd >= 0)
			return fd;
	}
	return -1;
}

bool
ts_temp_file(char* path, size_t cap, const char* tmpl)
{
	return make_temp(path, cap, tmpl);
}

bool
ts_write_config(ts_server_t* s, const char* tmpl, const char* text)
{
	FILE* file;

	if (!make_temp(s->config, sizeof(s->config), tmpl))
	{
		s->config[0] = '\0';
		return false;
	}
	file = fopen(s->config, "wb");
	if (!file)
	{
		unlink(s->config);
		s->config[0] = '\0';
		return false;
	}
	fputs(text, file);
	fclose(file);
	return true;
}

/* Processes. */

static HANDLE
kill_job(void)
{
	static HANDLE job;

	if (!job)
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = { 0 };

		job = CreateJobObjectA(NULL, NULL);
		if (job)
		{
			info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
		}
	}
	return job;
}

/* The command line of argv, quoted the way CommandLineToArgvW reads it back. */
static bool
command_line(char* const argv[], char* out, size_t cap)
{
	size_t used = 0;

	for (int i = 0; argv[i]; i++)
	{
		size_t slashes = 0;

		if (used + 3 >= cap)
			return false;
		if (i > 0)
			out[used++] = ' ';
		out[used++] = '"';
		for (const char* a = argv[i]; *a; a++)
		{
			char c = *a;

			/* CreateProcess wants backslashes in the program path. */
			if (i == 0 && c == '/')
				c = '\\';
			if (used + 2 * slashes + 4 >= cap)
				return false;
			if (c == '\\')
			{
				slashes++;
				continue;
			}
			if (c == '"')
				slashes = 2 * slashes + 1;
			for (; slashes > 0; slashes--)
				out[used++] = '\\';
			out[used++] = c;
		}
		if (used + 2 * slashes + 3 >= cap)
			return false;
		for (slashes *= 2; slashes > 0; slashes--)
			out[used++] = '\\';
		out[used++] = '"';
	}
	out[used] = '\0';
	return true;
}

/* Starts argv with the given standard handles (inheritable) and nothing else inherited. */
static bool
spawn(char* const argv[], HANDLE in, HANDLE out, HANDLE err, PROCESS_INFORMATION* pi)
{
	char			   line[4096];
	STARTUPINFOEXA		   si  = { 0 };
	SIZE_T			   len = 0;
	HANDLE			   list[3];
	DWORD			   count = 0;
	bool			   ok;

	if (!command_line(argv, line, sizeof(line)))
		return false;
	for (int i = 0; i < 3; i++)
	{
		HANDLE h = i == 0 ? in : i == 1 ? out : err;
		bool   seen = false;

		for (DWORD j = 0; j < count; j++)
			seen = seen || list[j] == h;
		if (!seen)
			list[count++] = h;
	}
	InitializeProcThreadAttributeList(NULL, 1, 0, &len);
	si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, len);
	if (!si.lpAttributeList ||
	    !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &len) ||
	    !UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list,
				       count * sizeof(HANDLE), NULL, NULL))
	{
		if (si.lpAttributeList)
			HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
		return false;
	}
	si.StartupInfo.cb         = sizeof(si);
	si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput  = in;
	si.StartupInfo.hStdOutput = out;
	si.StartupInfo.hStdError  = err;
	ok = CreateProcessA(NULL, line, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
			    NULL, NULL, &si.StartupInfo, pi) != 0;
	DeleteProcThreadAttributeList(si.lpAttributeList);
	HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
	if (ok)
	{
		HANDLE job = kill_job();

		if (job)
			AssignProcessToJobObject(job, pi->hProcess);
		CloseHandle(pi->hThread);
	}
	return ok;
}

/* An inheritable handle on a file that other processes may read and delete meanwhile. */
static HANDLE
open_file(const char* path, bool write)
{
	SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };

	return CreateFileA(path, write ? GENERIC_WRITE : GENERIC_READ,
			   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
			   write ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

static HANDLE
inherit_std(DWORD which)
{
	HANDLE h = GetStdHandle(which);

	if (h && h != INVALID_HANDLE_VALUE && SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
		return h;
	return open_file("NUL", which != STD_INPUT_HANDLE);
}

bool
ts_process_done(pid_t pid)
{
	return WaitForSingleObject(as_handle(pid), 0) == WAIT_OBJECT_0;
}

bool
ts_wait_pid(pid_t pid, int* status, u32 timeout_ms)
{
	HANDLE h    = as_handle(pid);
	DWORD  code = 0;
	bool   ok   = true;

	if (WaitForSingleObject(h, timeout_ms) != WAIT_OBJECT_0)
	{
		TerminateProcess(h, TS_KILLED_STATUS);
		ok = WaitForSingleObject(h, 5000) == WAIT_OBJECT_0;
	}
	GetExitCodeProcess(h, &code);
	if (status)
		*status = (int)code;
	CloseHandle(h);
	return ok;
}

/* Verbose runs keep stderr on the console, so use a connection probe instead. */
static bool
wait_ready_verbose(ts_server_t* s, pid_t pid)
{
	if (!s->probe_port)
	{
		Sleep(TS_START_FALLBACK_MS);
		return !ts_process_done(pid);
	}
	for (u32 i = 0; i < 300; i++)
	{
		int fd = ts_connect_loopback(s->probe_port, s->probe_family, 50);

		if (fd >= 0)
		{
			close(fd);
			Sleep(50);
			return true;
		}
		Sleep(10);
	}
	return false;
}

bool
ts_wait_startup(ts_server_t* s, pid_t pid)
{
	u64 deadline;

	if (!s->stderr_path[0])
		return wait_ready_verbose(s, pid);
	deadline = os_now_ms() + TS_START_TIMEOUT_MS;
	for (;;)
	{
		if (ts_file_contains(s->stderr_path, s->ready_needle))
			return true;	/* like kill(pid, 0) on an unreaped child: an exit right after is not our business */
		if (ts_process_done(pid))
		{
			if (s->flags & TS_EXIT_IS_READY)
				return true;
			fprintf(stderr, "%s: pid %d exited before readiness\n", s->label, (int)GetProcessId(as_handle(pid)));
			ts_dump_stderr(s);
			return false;
		}
		if (os_now_ms() >= deadline)
		{
			fprintf(stderr, "%s: pid %d never became ready\n", s->label, (int)GetProcessId(as_handle(pid)));
			ts_dump_stderr(s);
			return false;
		}
		Sleep(TS_READY_POLL_MS);
	}
}

static void
remove_files(ts_server_t* s)
{
	if (s->config[0])
		unlink(s->config);
	if (s->stderr_path[0])
		unlink(s->stderr_path);
	if (s->stdout_path[0])
		unlink(s->stdout_path);
	s->config[0]      = '\0';
	s->stderr_path[0] = '\0';
	s->stdout_path[0] = '\0';
	s->nofile         = 0;
}

bool
ts_server_start(ts_server_t* s, char* const argv[])
{
	bool		    capture_err = (s->flags & TS_CAPTURE_STDERR) && !ts_verbose();
	bool		    capture_out = (s->flags & TS_CAPTURE_STDOUT) && !ts_verbose();
	HANDLE		    in, out, err;
	PROCESS_INFORMATION pi = { 0 };
	bool		    ok;

	if (capture_err && !s->stderr_path[0] &&
	    !ts_temp_file(s->stderr_path, sizeof(s->stderr_path), "/tmp/mogbus-stderr-XXXXXX"))
		return false;
	if (capture_out && !s->stdout_path[0] &&
	    !ts_temp_file(s->stdout_path, sizeof(s->stdout_path), "/tmp/mogbus-stdout-XXXXXX"))
		goto fail;

	in  = (s->flags & TS_NULL_STDIN) ? open_file("NUL", false) : inherit_std(STD_INPUT_HANDLE);
	out = capture_out ? open_file(s->stdout_path, true) :
	      (s->flags & TS_NULL_STDOUT) ? open_file("NUL", true) : inherit_std(STD_OUTPUT_HANDLE);
	err = capture_err ? open_file(s->stderr_path, true) : inherit_std(STD_ERROR_HANDLE);
	ok  = in != INVALID_HANDLE_VALUE && out != INVALID_HANDLE_VALUE && err != INVALID_HANDLE_VALUE &&
	      spawn(argv, in, out, err, &pi);
	if (in != INVALID_HANDLE_VALUE && (s->flags & TS_NULL_STDIN))
		CloseHandle(in);
	if (out != INVALID_HANDLE_VALUE && (capture_out || (s->flags & TS_NULL_STDOUT)))
		CloseHandle(out);
	if (err != INVALID_HANDLE_VALUE && capture_err)
		CloseHandle(err);
	if (!ok)
		goto fail;
	s->pid = from_handle(pi.hProcess);
	if (ts_wait_startup(s, s->pid))
		return true;
	(void)ts_server_stop(s);
	return false;

fail:
	remove_files(s);
	return false;
}

/* Asks the process to stop through its named event; false while the event does not exist yet. */
static bool
signal_stop(HANDLE h)
{
	char   name[48];
	HANDLE event;

	snprintf(name, sizeof(name), "Local\\mogbus-stop-%lu", (unsigned long)GetProcessId(h));
	event = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
	if (!event)
		return false;
	SetEvent(event);
	CloseHandle(event);
	return true;
}

/* Stops the process and returns its exit status; *forced says it had to be terminated. */
static DWORD
stop_process(HANDLE h, bool* forced)
{
	DWORD code = 0;
	u64   deadline = os_now_ms() + TS_STOP_ATTEMPTS * TS_STOP_POLL_US / 1000;
	u64   now;

	*forced = false;
	/* The event exists before the server listens; a process that died early never had one. */
	while (WaitForSingleObject(h, 0) != WAIT_OBJECT_0 && !signal_stop(h) && os_now_ms() < deadline)
		Sleep(5);
	now = os_now_ms();
	if (WaitForSingleObject(h, now < deadline ? (DWORD)(deadline - now) : 0) != WAIT_OBJECT_0)
	{
		TerminateProcess(h, TS_KILLED_STATUS);
		WaitForSingleObject(h, 5000);
		*forced = true;
	}
	GetExitCodeProcess(h, &code);
	return code;
}

bool
ts_server_stop(ts_server_t* s)
{
	bool  ok     = true;
	pid_t pid    = s->pid;

	s->pid = -1;
	if (pid > 0)
	{
		bool  forced;
		DWORD code = stop_process(as_handle(pid), &forced);

		ok = (s->flags & TS_CLEAN_EXIT) ? !forced && code == 0 : forced || code == 0;
		if (!ok)
		{
			ts_dump_stderr(s);
			fprintf(stderr, "%s: pid %d exit code %lu forced=%d\n", s->label, (int)pid,
				(unsigned long)code, (int)forced);
		}
		CloseHandle(as_handle(pid));
	}
	remove_files(s);
	return ok;
}

void
ts_server_cleanup(ts_server_t* s)
{
	pid_t pid = s->pid;

	s->pid = -1;
	if (pid > 0)
	{
		bool forced;

		(void)stop_process(as_handle(pid), &forced);
		CloseHandle(as_handle(pid));
	}
	remove_files(s);
}

int
ts_exec(char* const argv[], const char* input, char* out, size_t cap)
{
	SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
	HANDLE		    in_r, in_w, out_r, out_w;
	PROCESS_INFORMATION pi = { 0 };
	size_t		    used = 0;
	DWORD		    got, code = 0;

	out[0] = '\0';
	if (!CreatePipe(&in_r, &in_w, &sa, 0))
		return -1;
	if (!CreatePipe(&out_r, &out_w, &sa, 0))
	{
		CloseHandle(in_r);
		CloseHandle(in_w);
		return -1;
	}
	SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
	if (!spawn(argv, in_r, out_w, out_w, &pi))
		code = (DWORD)-1;
	CloseHandle(in_r);
	CloseHandle(out_w);
	if (code != (DWORD)-1)
	{
		DWORD wrote;

		if (input && input[0])
			WriteFile(in_w, input, (DWORD)strlen(input), &wrote, NULL);
		CloseHandle(in_w);
		while (used + 1 < cap && ReadFile(out_r, out + used, (DWORD)(cap - 1 - used), &got, NULL) && got > 0)
			used += got;
		/* Drain what does not fit so the child never blocks on a full pipe. */
		{
			char sink[512];

			while (ReadFile(out_r, sink, sizeof(sink), &got, NULL) && got > 0)
				;
		}
		WaitForSingleObject(pi.hProcess, 30000);
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hProcess);
	}
	else
		CloseHandle(in_w);
	CloseHandle(out_r);
	out[used] = '\0';
	/* Text mode of a Unix test: no carriage returns. */
	for (size_t i = 0, j = 0;; i++)
	{
		if (out[i] != '\r')
			out[j++] = out[i];
		if (!out[i])
			break;
	}
	return (int)code;
}
#endif
