/* mogbus --show-registers: run the real binary, change a register over Modbus TCP and read the
 * values it prints to stdout. Each test uses its own port, so tests may run in parallel. */
#include "vtest.h"
#include "modbus/pdu.h"
#include "os/os_time.h"
#include "tests/tests_utils/test_support.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static ts_server_t g_show = {
	.pid		= -1,
	.master_fd	= -1,
	.probe_family	= AF_INET,
	.flags		= TS_CAPTURE_STDERR | TS_CAPTURE_STDOUT | TS_NULL_STDIN | TS_CLEAN_EXIT,
	.label		= "show registers",
	.ready_needle	= "listening on"
};

static const char* const g_devices =
	"(device (unit-id 7)\n"
	"  (holding 10 (static 5))\n"
	"  (holding 11 (type float32) (static 1.5))\n"
	"  (holding 20 (type string) (length 2) (static \"Hi\"))\n"
	"  (coil 0 (static 1)))\n";

SETUP_SUITE()
{
	(void)_ss_arena;
}

PRE_TEST()
{
	ts_server_cleanup(&g_show);
}

static const char*
mogbus_bin(void)
{
	const char* bin = getenv("MOGBUS_BIN");

	return bin ? bin : "./mogbus";
}

static bool
show_write_config(void)
{
	char config[512];

	g_show.probe_port = ts_free_port();
	if (!g_show.probe_port)
		return false;
	snprintf(config, sizeof(config), "(server (mode tcp) (port %u))\n%s", g_show.probe_port, g_devices);
	return ts_write_config(&g_show, "/tmp/mogbus-show-XXXXXX", config);
}

static bool
show_start(const char* flag, bool json)
{
	char*	argv[6];
	int	argc = 0;

	if (!show_write_config())
		return false;
	argv[argc++] = (char*)mogbus_bin();
	argv[argc++] = (char*)flag;
	if (json)
	{
		argv[argc++] = "--log-format";
		argv[argc++] = "json";
	}
	argv[argc++] = g_show.config;
	argv[argc]   = NULL;
	return ts_server_start(&g_show, argv);
}

/* Polls the captured stdout until it contains the text or 5 s pass. */
static bool
show_wait(const char* needle)
{
	u64 deadline = os_now_ms() + 5000;

	while (!ts_file_contains(g_show.stdout_path, needle))
	{
		if (os_now_ms() >= deadline)
			return false;
		usleep(10000);
	}
	return true;
}

/* Number of stdout lines that contain the text. */
static int
show_count(const char* needle)
{
	FILE*	file = fopen(g_show.stdout_path, "r");
	char	line[512];
	int	count = 0;

	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file))
		count += strstr(line, needle) != NULL;
	fclose(file);
	return count;
}

static bool
show_write_holding(u16 address, u16 value)
{
	int	fd  = ts_connect_loopback(g_show.probe_port, AF_INET, 1000);
	u16	tid = ts_next_tid();
	u8	req[5] = { 0x06, (u8)(address >> 8), (u8)address, (u8)(value >> 8), (u8)value };
	u8	resp[MODBUS_MAX_PDU];
	u32	len = 0;
	bool	ok;

	if (fd < 0)
		return false;
	ok = ts_mbap_send(fd, 7, tid, req, sizeof(req)) &&
	     ts_mbap_recv(fd, 7, tid, resp, &len, 1000) &&
	     len == 5 &&
	     resp[0] == 0x06;
	close(fd);
	return ok;
}

TEST(ShowRegistersPrintsSnapshotThenOnlyChanges)
{
	bool ok = show_start("--show-registers=50", false);

	ok = ok &&
	     show_wait("unit 7 holding 10 = 5") &&
	     show_wait("unit 7 holding 11 = 1.5") &&
	     show_wait("unit 7 holding 20 = \"Hi\"") &&
	     show_wait("unit 7 coil 0 = 1") &&
	     show_write_holding(10, 42) &&
	     show_wait("unit 7 holding 10 = 42");
	/* Let several more intervals pass: nothing unchanged may be printed again. */
	usleep(300000);
	ok = ok &&
	     show_count("holding 10 = 5") == 1 &&
	     show_count("holding 10 = 42") == 1 &&
	     show_count("holding 11 = 1.5") == 1 &&
	     show_count("coil 0 = 1") == 1;
	ASSERT_EQ(ts_server_stop(&g_show) && ok);
}

TEST(ShowRegistersPrintsJsonLinesWithLogFormatJson)
{
	bool ok = show_start("-r", true);

	ok = ok &&
	     show_wait("{\"unit\":7,\"space\":\"holding\",\"address\":10,\"value\":5}") &&
	     show_wait("{\"unit\":7,\"space\":\"holding\",\"address\":20,\"value\":\"Hi\"}") &&
	     show_write_holding(10, 43) &&
	     show_wait("{\"unit\":7,\"space\":\"holding\",\"address\":10,\"value\":43}");
	ASSERT_EQ(ts_server_stop(&g_show) && ok);
}

static int
cli_status(const char* flag, const char* config)
{
	char*	argv[] = { (char*)mogbus_bin(), (char*)flag, (char*)config, NULL };
	int	status = 0;
	pid_t	pid    = fork();

	if (pid == 0)
	{
		if (!freopen("/dev/null", "w", stderr))
			_exit(126);
		execv(argv[0], argv);
		_exit(127);
	}
	if (pid < 0 || !ts_wait_pid(pid, &status, 5000) || !WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
}

TEST(ShowRegistersRejectsAnIntervalOutOfRange)
{
	bool ok = show_write_config();

	ok = ok &&
	     cli_status("--show-registers=9", g_show.config) == 2 &&
	     cli_status("--show-registers=60001", g_show.config) == 2 &&
	     cli_status("--show-registers=abc", g_show.config) == 2 &&
	     cli_status("--show-registers=", g_show.config) == 2;
	ASSERT_EQ(ts_server_stop(&g_show) && ok);
}
