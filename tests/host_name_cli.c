/* mogbus --check with host names in the listen address: the real binary resolves them. */
#include "vtest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static char g_out[4096];

/* Writes `config` to a temp file and runs `<binary> --check <file>`; returns the exit status. */
static int
check_config(const char* config)
{
	const char*	bin = getenv("MOGBUS_BIN");
	char		path[] = "/tmp/mogbus-hostname-XXXXXX";
	char		command[512];
	FILE*		file;
	FILE*		pipe;
	size_t		used = 0;
	int		fd = mkstemp(path);
	int		status;

	if (fd < 0)
		return -1;
	file = fdopen(fd, "w");
	fputs(config, file);
	fclose(file);
	snprintf(command, sizeof(command), "%s --check %s 2>&1", bin ? bin : "./mogbus", path);
	pipe = popen(command, "r");
	if (pipe)
	{
		used   = fread(g_out, 1, sizeof(g_out) - 1, pipe);
		status = pclose(pipe);
	}
	else
		status = -1;
	g_out[used] = '\0';
	remove(path);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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
