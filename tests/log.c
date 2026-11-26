/* Process-wide log sink and level. Each test runs in its own process. */
#include "vtest.h"
#include "util/log.h"
#include <stdio.h>
#include <string.h>

typedef struct capture_t capture_t;
struct capture_t
{
	u32		count;
	log_level_t	level;
	char		last[1100];
};

static void
capture(void* ctx, log_level_t level, const char* message)
{
	capture_t* c = ctx;

	c->count++;
	c->level = level;
	snprintf(c->last, sizeof(c->last), "%s", message);
}

TEST(LogSinkReceivesFormattedLines)
{
	capture_t c = { 0 };

	log_set_sink(capture, &c);
	LOG_INFO("listening on %s:%u", "127.0.0.1", 1502u);
	ASSERT_EQ(c.count == 1 && c.level == LOG_LEVEL_INFO);
	ASSERT_EQ(strcmp(c.last, "listening on 127.0.0.1:1502") == 0);
	LOG_WARN("bad %d", 7);
	ASSERT_EQ(c.count == 2 && c.level == LOG_LEVEL_WARN && strcmp(c.last, "bad 7") == 0);
	log_set_sink(NULL, NULL);
	LOG_WARN("back to stderr");
	ASSERT_EQ(c.count == 2);
}

TEST(LogLevelFiltersLines)
{
	capture_t c = { 0 };

	log_set_sink(capture, &c);
	LOG_DEBUG("hidden by default");
	ASSERT_EQ(c.count == 0);
	log_set_level(LOG_LEVEL_DEBUG);
	LOG_DEBUG("shown");
	ASSERT_EQ(c.count == 1 && c.level == LOG_LEVEL_DEBUG);
	log_set_level(LOG_LEVEL_WARN);
	LOG_INFO("hidden");
	LOG_WARN("shown");
	ASSERT_EQ(c.count == 2);
	log_set_level(LOG_LEVEL_OFF);
	LOG_WARN("hidden");
	log_write(LOG_LEVEL_OFF, "never a real level");
	ASSERT_EQ(c.count == 2);
	log_set_sink(NULL, NULL);
}

TEST(LogTruncatesLongMessages)
{
	capture_t	c = { 0 };
	char		big[2000];

	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	log_set_sink(capture, &c);
	LOG_INFO("%s", big);
	ASSERT_EQ(c.count == 1 && strlen(c.last) == 1023);
	log_set_sink(NULL, NULL);
}

TEST(LogJsonFormatEscapesAndAddsFields)
{
	capture_t c = { 0 };

	log_set_sink(capture, &c);
	log_set_format(LOG_FORMAT_JSON);
	LOG_INFO("name \"%s\"\tback\\slash", "x");
	ASSERT_EQ(c.count == 1);
	ASSERT_EQ(strncmp(c.last, "{\"ts\":\"", 7) == 0);
	ASSERT_EQ(strstr(c.last, "\"level\":\"info\",\"msg\":\"name \\\"x\\\"\\u0009back\\\\slash\"}") != NULL);
	log_set_level(LOG_LEVEL_DEBUG);
	log_write_fields(LOG_LEVEL_DEBUG, "\"event\":\"request\",\"unit\":1", "req");
	ASSERT_EQ(strstr(c.last, "\"msg\":\"req\",\"event\":\"request\",\"unit\":1}") != NULL);
	log_set_format(LOG_FORMAT_TEXT);
	log_write_fields(LOG_LEVEL_DEBUG, "\"event\":\"request\"", "plain %d", 5);
	ASSERT_EQ(strcmp(c.last, "plain 5") == 0);
	log_set_sink(NULL, NULL);
}

TEST(LogJsonEscapeTruncatesSafely)
{
	char out[8];

	ASSERT_EQ(log_json_escape(out, sizeof(out), "a\"b") == 4 && strcmp(out, "a\\\"b") == 0);
	ASSERT_EQ(log_json_escape(out, sizeof(out), "abcdefghij") == 7 && strcmp(out, "abcdefg") == 0);
	ASSERT_EQ(log_json_escape(out, 4, "\n") == 0 && out[0] == '\0');	/* \u000a does not fit */
}
