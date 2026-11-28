/** @file
 *  @brief Test-only POSIX socket and process names on Windows (MinGW-w64), so the integration
 *         tests build unchanged. Included through the shims in compat/ (`<sys/socket.h>`, `<poll.h>`,
 *         ...); on POSIX this file is empty. Sockets are plain `int`s in the tests (Windows hands
 *         out small handle values), so close/read/write dispatch on whether the descriptor is a socket.
 */
#ifndef TESTS_TESTS_UTILS_TEST_OS_H
#define TESTS_TESTS_UTILS_TEST_OS_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN	/* no OLE: its VT_BOOL clashes with vtest.h */
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int	ts_close(int fd);
int	ts_read(int fd, void* buf, unsigned len);
int	ts_write(int fd, const void* buf, unsigned len);
int	ts_mkstemp(char* tmpl);
int	ts_socketpair(int domain, int type, int protocol, int sv[2]);

#undef usleep
#define usleep(us) Sleep((DWORD)(((us) + 999) / 1000))
#define mkstemp ts_mkstemp
#define socketpair ts_socketpair
#define SHUT_WR SD_SEND
#define SHUT_RD SD_RECEIVE
#define SHUT_RDWR SD_BOTH
typedef ULONG nfds_t;
#define close ts_close
#define read ts_read
#define write ts_write
#define poll(fds, n, timeout) WSAPoll((fds), (ULONG)(n), (timeout))
#define setsockopt(s, level, name, val, len) setsockopt((s), (level), (name), (const char*)(val), (len))
#define getsockopt(s, level, name, val, len) getsockopt((s), (level), (name), (char*)(val), (len))
#define send(s, buf, len, flags) send((s), (const char*)(buf), (int)(len), (flags))
#define recv(s, buf, len, flags) recv((s), (char*)(buf), (int)(len), (flags))
#define sendto(s, buf, len, flags, to, tolen) sendto((s), (const char*)(buf), (int)(len), (flags), (to), (tolen))
#define recvfrom(s, buf, len, flags, from, fromlen) recvfrom((s), (char*)(buf), (int)(len), (flags), (from), (fromlen))

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* <winbase.h> macros that collide with enumerators of the tests. */
#undef PARITY_NONE
#undef PARITY_ODD
#undef PARITY_EVEN
#undef PARITY_MARK
#undef PARITY_SPACE

/* <excpt.h> macros that collide with member names of the tests. */
#undef ERROR
#undef exception_code
#undef exception_info
#undef abnormal_termination

/* Wait statuses are plain exit codes (see ts_wait_pid()). */
#define WIFEXITED(status) 1
#define WEXITSTATUS(status) (status)
#define WIFSIGNALED(status) 0
#define WTERMSIG(status) 0
#endif

#endif
