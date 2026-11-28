/* mogbus --decode: run the real binary on frames given as arguments and on stdin, and check the
 * text, the JSON lines and the exit status. */
#include "vtest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tests/tests_utils/test_support.h"

static char g_out[65536];

/* Runs `<binary> <args>` through the shell with `input` (printf %b escapes) on stdin, capturing
 * stdout and stderr; returns the exit status. */
static int
run_cli_in(const char* input, const char* args)
{
	const char*	bin = getenv("MOGBUS_BIN");
	char		command[2048];
	char		text[512];
	char*		argv[32];
	size_t		n = 0;

	/* printf %b: the escapes of `input` become bytes. */
	for (const char* c = input; *c && n + 1 < sizeof(text); c++)
	{
		if (c[0] == '\\' && c[1] == 'n')
			text[n++] = '\n', c++;
		else if (c[0] == '\\' && c[1] == 'r')
			text[n++] = '\r', c++;
		else
			text[n++] = *c;
	}
	text[n] = '\0';
	snprintf(command, sizeof(command), "%s %s", bin ? bin : "./mogbus", args);
	ts_split_args(command, argv, 32);
	return ts_exec(argv, text, g_out, sizeof(g_out));
}

static int
run_cli(const char* args)
{
	return run_cli_in("", args);
}

TEST(DecodeCliPrintsAnMbapRequest)
{
	ASSERT_EQ(run_cli("--decode '00 01 00 00 00 06 01 03 00 00 00 02'") == 0);
	ASSERT_EQ(strstr(g_out, "framing: mbap") != NULL);
	ASSERT_EQ(strstr(g_out, "transaction: 1 (0x0001)") != NULL);
	ASSERT_EQ(strstr(g_out, "function: 3 (0x03) Read Holding Registers") != NULL);
	ASSERT_EQ(strstr(g_out, "direction: request") != NULL);
	ASSERT_EQ(strstr(g_out, "quantity: 2") != NULL);
	ASSERT_EQ(strstr(g_out, "ok: true") != NULL);
}

TEST(DecodeCliAcceptsHexSpellingsAndAsciiText)
{
	ASSERT_EQ(run_cli("--decode 010300000001840A") == 0);
	ASSERT_EQ(strstr(g_out, "framing: rtu") != NULL);
	ASSERT_EQ(run_cli("--decode 01:03:00:00:00:01:84:0A") == 0);
	ASSERT_EQ(strstr(g_out, "crc_ok: true") != NULL);
	ASSERT_EQ(run_cli("--decode 0x01,0x03,0x00,0x00,0x00,0x01,0x84,0x0A") == 0);
	ASSERT_EQ(strstr(g_out, "crc_ok: true") != NULL);
	ASSERT_EQ(run_cli("--decode ':010300000001FB'") == 0);
	ASSERT_EQ(strstr(g_out, "framing: ascii") != NULL);
	ASSERT_EQ(strstr(g_out, "lrc_ok: true") != NULL);
	ASSERT_EQ(run_cli("--decode ':010300000001FB\\r\\n'") == 0);
	ASSERT_EQ(strstr(g_out, "lrc_ok: true") != NULL);
}

TEST(DecodeCliHonoursFramingAndDirection)
{
	ASSERT_EQ(run_cli("--decode --framing rtu --direction response '01 03 02 00 0A 38 43'") == 0);
	ASSERT_EQ(strstr(g_out, "values: 10") != NULL);
	ASSERT_EQ(strstr(g_out, "direction: response") != NULL);
	ASSERT_EQ(strstr(g_out, "direction_reason") == NULL);
	ASSERT_EQ(run_cli("--decode --framing mbap '01 03 02 00 0A 38 43'") == 1);
	ASSERT_EQ(strstr(g_out, "framing: mbap") != NULL);
}

TEST(DecodeCliExitStatusIsOneForAMalformedFrame)
{
	ASSERT_EQ(run_cli("--decode '01 03 00 00 00 01 84 0B'") == 1);
	ASSERT_EQ(strstr(g_out, "crc_ok: false") != NULL);
	ASSERT_EQ(strstr(g_out, "CRC mismatch") != NULL);
	ASSERT_EQ(strstr(g_out, "ok: false") != NULL);
	/* the good frame still prints and the exit status is the worst of the frames */
	ASSERT_EQ(run_cli("--decode '01 03 00 00 00 01 84 0A' '01 03 00 00 00 01 84 0B'") == 1);
	ASSERT_EQ(strstr(g_out, "ok: true") != NULL);
	ASSERT_EQ(strstr(g_out, "ok: false") != NULL);
}

TEST(DecodeCliPrintsJsonLinesWithLogFormat)
{
	char*	second;

	ASSERT_EQ(run_cli("--decode --log-format json '01 03 00 00 00 01 84 0A' '01 83 02 C0 F1'") == 0);
	ASSERT_EQ(g_out[0] == '{');
	second = strchr(g_out, '\n');
	ASSERT_EQ(second != NULL);
	ASSERT_EQ(second[1] == '{');
	ASSERT_EQ(strstr(g_out, "\"function_name\":\"Read Holding Registers\"") != NULL);
	ASSERT_EQ(strstr(g_out, "\"exception_name\":\"Illegal Data Address\"") != NULL);
	ASSERT_EQ(strchr(second + 1, '\n') != NULL);
	ASSERT_EQ(strchr(strchr(second + 1, '\n') + 1, '\n') == NULL);
}

TEST(DecodeCliReadsFramesFromStdin)
{
	ASSERT_EQ(run_cli_in("# a comment\\n01 03 00 00 00 01 84 0A\\n\\n:010300000001FB\\n", "--decode") == 0);
	ASSERT_EQ(strstr(g_out, "framing: rtu") != NULL);
	ASSERT_EQ(strstr(g_out, "framing: ascii") != NULL);
	ASSERT_EQ(strstr(g_out, "# a comment") == NULL);
}

TEST(DecodeCliRejectsUnreadableInput)
{
	ASSERT_EQ(run_cli("--decode 'zz'") == 2);
	ASSERT_EQ(strstr(g_out, "invalid character 'z'") != NULL);
	ASSERT_EQ(run_cli("--decode '0'") == 2);
	ASSERT_EQ(strstr(g_out, "odd number of hex digits") != NULL);
	ASSERT_EQ(run_cli("--decode --framing nope 01") == 2);
	ASSERT_EQ(strstr(g_out, "--framing needs") != NULL);
	ASSERT_EQ(run_cli("--decode --direction sideways 01") == 2);
	ASSERT_EQ(run_cli("--framing rtu profiles/minimal.scm") == 2);
	ASSERT_EQ(strstr(g_out, "need --decode") != NULL);
}

TEST(DecodeCliWithoutArgumentsStillNeedsAConfig)
{
	ASSERT_EQ(run_cli("") == 2);
	ASSERT_EQ(strstr(g_out, "Usage:") != NULL);
	ASSERT_EQ(strstr(g_out, "--decode") != NULL);
	ASSERT_EQ(run_cli("--check profiles/minimal.scm") == 0);
}
