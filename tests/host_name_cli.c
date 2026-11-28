/* mogbus --check with host names in the listen address: the real binary resolves them. */
#include "vtest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tests_utils/test_support.h"
#include <unistd.h>

static char g_out[4096];

/* Writes `config` to a temp file and runs `<binary> --check <file>`; returns the exit status. */
static int
check_config(const char* config)
{
	const char*	bin = getenv("MOGBUS_BIN");
	ts_server_t	file;
	char*		argv[] = { (char*)(bin ? bin : "./mogbus"), "--check", file.config, NULL };
	int		status;

	ts_server_init(&file, "hostname");
	if (!ts_write_config(&file, "/tmp/mogbus-hostname-XXXXXX", config))
		return -1;
	status = ts_exec(argv, NULL, g_out, sizeof(g_out));
	unlink(file.config);
	return status;
}

#define DEV " (device (unit-id 1) (holding 0 (static 1)))"

TEST(HostNameCheckResolvesServerAndHttpAddresses)
{
	ASSERT_EQ(check_config("(server (mode tcp) (bind \"localhost\") (port 1502))" DEV) == 0);
	ASSERT_EQ(g_out[0] == '\0');
	ASSERT_EQ(check_config("(server (mode udp) (bind \"localhost\") (port 1502))" DEV) == 0);
	ASSERT_EQ(check_config("(server (mode tcp) (http (bind \"localhost\") (port 9102)))" DEV) == 0);
}

TEST(HostNameCheckRejectsUnresolvableAndMalformedNames)
{
	ASSERT_EQ(check_config("(server (mode tcp) (bind \"mogbus-no-such-host.invalid\"))" DEV) == 1);
	ASSERT_EQ(strstr(g_out, "bind address 'mogbus-no-such-host.invalid' is not an IP address and does not resolve") != NULL);
	ASSERT_EQ(check_config("(server (mode tcp) (http (bind \"mogbus-no-such-host.invalid\")))" DEV) == 1);
	ASSERT_EQ(strstr(g_out, "HTTP bind address 'mogbus-no-such-host.invalid'") != NULL);
	ASSERT_EQ(check_config("(server (mode tcp) (bind \"bad host!\"))" DEV) != 0);
	ASSERT_EQ(strstr(g_out, "or a host name") != NULL);
}
