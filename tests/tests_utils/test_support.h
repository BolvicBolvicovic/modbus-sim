#ifndef TESTS_TESTS_UTILS_TEST_SUPPORT_H
#define TESTS_TESTS_UTILS_TEST_SUPPORT_H

#include "modbus/mbap.h"
#include "tests/tests_utils/test_os.h"
#include "vlib/bits.h"
#include "vlib/c_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* Shared infrastructure for integration tests that run the real server binary.
 * Everything here is assertion-free: suites keep their own ASSERT wrappers. */
enum
{
	TS_READY_POLL_MS	= 2,
	TS_START_TIMEOUT_MS	= 5000,
	TS_START_FALLBACK_MS	= 300,
	TS_STOP_ATTEMPTS	= 200,
	TS_STOP_POLL_US		= 10000
};

enum
{
	TS_CAPTURE_STDERR	= BIT0,
	TS_CAPTURE_STDOUT	= BIT1,
	TS_NULL_STDIN		= BIT2,
	TS_NULL_STDOUT		= BIT3,
	TS_DEATH_SIGNAL		= BIT4,
	TS_CLEAN_EXIT		= BIT5,
	TS_EXIT_IS_READY	= BIT6
};

typedef struct ts_server ts_server_t;
struct ts_server
{
	pid_t	pid;		/* the process id; on Windows the process HANDLE (32 significant bits) */
	int	master_fd;
	u16	probe_port;	/* set for network fixtures: fallback readiness probe in verbose runs */
	int	probe_family;
	u32	nofile;
	u32	flags;
	char	label[24];
	char	ready_needle[64];
	char	config[80];
	char	stderr_path[80];
	char	stdout_path[80];
};

void	ts_server_init(ts_server_t* s, const char* label);
bool	ts_temp_file(char* path, size_t cap, const char* tmpl);
bool	ts_write_config(ts_server_t* s, const char* tmpl, const char* text);
bool	ts_server_start(ts_server_t* s, char* const argv[]);
bool	ts_server_stop(ts_server_t* s);		/* asserts via the caller's return value */
void	ts_server_cleanup(ts_server_t* s);		/* best effort, never asserts */

/* Runs argv without a shell, `input` (may be NULL) on stdin; stdout and stderr, merged, land in
 * out (NUL-terminated, truncated at cap). Returns the exit status, or -1 when it did not run. */
int	ts_exec(char* const argv[], const char* input, char* out, size_t cap);

/* Splits line in place into argv (at most max - 1 words, NULL-terminated): words are separated by
 * blanks and a '...' group keeps its blanks (no escapes). Returns the word count. */
int	ts_split_args(char* line, char* argv[], int max);

bool	ts_verbose(void);
void	ts_dump_stderr(const ts_server_t* s);
bool	ts_file_contains(const char* path, const char* needle);
bool	ts_process_done(pid_t pid);
bool	ts_wait_startup(ts_server_t* s, pid_t pid);
bool	ts_wait_pid(pid_t pid, int* status, u32 timeout_ms);
bool	ts_wait_eof(int fd, u32 timeout_ms);

/* fd and socket I/O */
bool	ts_send_fd(int fd, const u8* buf, u32 len);
bool	ts_send_socket(int fd, const u8* buf, u32 len);
void	ts_set_recv_timeout(int fd, u32 timeout_ms);
bool	ts_recv_exact(int fd, u8* buf, u32 len, u32 timeout_ms);
bool	ts_no_data(int fd, u32 timeout_ms);

/* loopback/network */
u16	ts_free_port(void);
bool	ts_ipv6_loopback_available(void);
int	ts_connect_loopback(u16 port, int family, u32 timeout_ms);

/* UDP: a connected datagram socket to the loopback port; one datagram per recv */
int	ts_udp_loopback(u16 port, int family, u32 timeout_ms);
s32	ts_udp_recv(int fd, u8* buf, u32 cap, u32 timeout_ms);	/* length, or -1 on timeout */
bool	ts_udp_mbap_recv(int fd, u8 unit, u16 tid, u8* pdu, u32* pdu_len, u32 timeout_ms);

/* wire frames; `on_socket` selects send()/recv() instead of write()/poll() */
u16	ts_next_tid(void);
u32	ts_mbap_encode(u8* frame, u8 unit, u16 tid, const u8* pdu, u32 pdu_len);
bool	ts_mbap_reply_header(const u8* raw, u8 unit, u16 tid, u32* pdu_len);
bool	ts_mbap_send(int fd, u8 unit, u16 tid, const u8* pdu, u32 pdu_len);
bool	ts_mbap_recv(int fd, u8 unit, u16 tid, u8* pdu, u32* pdu_len, u32 timeout_ms);
bool	ts_rtu_send(int fd, u8 addr, const u8* pdu, u32 pdu_len, bool on_socket);
bool	ts_rtu_recv(int fd, u8* addr, u8* pdu, u32* pdu_len, u32 timeout_ms, bool on_socket);
bool	ts_ascii_send(int fd, u8 addr, const u8* pdu, u32 pdu_len, bool on_socket);
bool	ts_ascii_recv_frame(int fd, u8* frame, u32* len, u32 timeout_ms, bool on_socket);
bool	ts_ascii_recv_response(int fd, u8* addr, u8* pdu, u32* pdu_len, u32 timeout_ms,
			       bool on_socket);
bool	ts_ascii_recv_frames(int fd, u32 expected, u8* addrs, u8 (*pdus)[MODBUS_MAX_PDU],
			     u32* lens, u32 timeout_ms);

#endif
