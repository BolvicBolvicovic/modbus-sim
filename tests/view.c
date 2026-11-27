/* The live register view: the table drawn for a fake terminal size and the line output. */
#include "vtest.h"
#include "mogbus.h"
#include "view/view.h"
#include <stdio.h>
#include <string.h>

static device_t*
make_device(mogbus_t* ctx, uint8_t unit, uint32_t points)
{
	mogbus_device_t* dev = NULL;

	mogbus_add_device(ctx, unit, "d", &dev);
	for (uint32_t i = 0; i < points; i++)
	{
		mogbus_point_t p = { .space = MOGBUS_HOLDING, .address = i, .type = MOGBUS_TYPE_U16, .value = 100 + i };

		mogbus_device_add_point(dev, &p);
	}
	return (device_t*)dev;
}

static u32
count_char(const char* text, char c)
{
	u32 n = 0;

	for (; *text; text++)
		n += *text == c;
	return n;
}

TEST(ViewTableShowsHeaderRateAndPoints)
{
	static char	out[VIEW_FRAME_MAX];
	mogbus_t*	ctx = NULL;
	device_t*	devices[1];
	view_t		view = { .interval_ms = 250, .table = true };

	mogbus_create(&ctx);
	devices[0] = make_device(ctx, 7, 3);
	view_render_table(&view, devices, 1, 24, 80, 1000, out, sizeof(out));
	ASSERT_EQ(strstr(out, "mogbus  1 device, 3 points  refresh 250 ms, achieved -") != NULL);
	ASSERT_EQ(strstr(out, "UNIT  TABLE") != NULL);
	ASSERT_EQ(strstr(out, "7     holding       1  u16     101") != NULL);
	ASSERT_EQ(view.height == 5);
	ASSERT_EQ(out[0] == '\r' && strstr(out, "\x1b[J") != NULL);
	ASSERT_EQ(strstr(out, "more point") == NULL);

	/* The second frame moves back over the first one and reports the achieved rate. */
	view.last_ms = 1000;
	view_render_table(&view, devices, 1, 24, 80, 1500, out, sizeof(out));
	ASSERT_EQ(strncmp(out, "\r\x1b[4A", 5) == 0);
	ASSERT_EQ(strstr(out, "achieved 2.00/s") != NULL);
	mogbus_destroy(ctx);
}

TEST(ViewTableFitsTheTerminalHeight)
{
	static char	out[VIEW_FRAME_MAX];
	mogbus_t*	ctx = NULL;
	device_t*	devices[1];
	view_t		view = { .interval_ms = 1000, .table = true };

	mogbus_create(&ctx);
	devices[0] = make_device(ctx, 1, 40);
	view_render_table(&view, devices, 1, 10, 80, 1000, out, sizeof(out));
	ASSERT_EQ(view.height == 10);
	ASSERT_EQ(count_char(out, '\n') == 9);
	ASSERT_EQ(strstr(out, "... 33 more points") != NULL);
	ASSERT_EQ(strstr(out, "holding       6  u16") != NULL && strstr(out, "holding       7  u16") == NULL);
	mogbus_destroy(ctx);
}

TEST(ViewTableCutsLinesToTheWidthAndKeepsALogPane)
{
	static char	out[VIEW_FRAME_MAX];
	mogbus_t*	ctx = NULL;
	device_t*	devices[1];
	view_t		view = { .interval_ms = 1000, .table = true };

	mogbus_create(&ctx);
	devices[0] = make_device(ctx, 1, 2);
	snprintf(view.log[0], VIEW_LOG_WIDTH, "[warn] something went wrong");
	view.log_count = 1;
	view.log_next  = 1;
	view_render_table(&view, devices, 1, 20, 30, 1000, out, sizeof(out));
	ASSERT_EQ(strstr(out, "-- log --") != NULL && strstr(out, "[warn] something went wrong") != NULL);
	ASSERT_EQ(strstr(out, "mogbus  1 device, 2 points  r") != NULL && strstr(out, "refresh") == NULL);
	ASSERT_EQ(view.height == 2 + 2 + 2 && view.pane == 2);
	/* Closing erases the pane (its lines go to stderr once) and shows the cursor. */
	view_render_close(&view, out, sizeof(out));
	ASSERT_EQ(strcmp(out, "\r\x1b[1A\x1b[J\x1b[?25h") == 0);
	view.pane = 0;
	view_render_close(&view, out, sizeof(out));
	ASSERT_EQ(strcmp(out, "\n\x1b[?25h") == 0);
	mogbus_destroy(ctx);
}

TEST(ViewLinesPrintTheSnapshotThenOnlyChanges)
{
	static u64	shown[VIEW_MAX_DEVICES][DEVICE_MAX_POINTS];
	mogbus_t*	ctx = NULL;
	device_t*	devices[1];
	view_t		view = { .interval_ms = 1000, .shown = shown };
	char		text[512];
	FILE*		f = tmpfile();
	size_t		n;

	mogbus_create(&ctx);
	devices[0] = make_device(ctx, 7, 2);
	view_render_lines(&view, devices, 1, true, 1000, f);
	mogbus_device_set_value((mogbus_device_t*)devices[0], MOGBUS_HOLDING, 1, 5);
	view_render_lines(&view, devices, 1, false, 2000, f);
	rewind(f);
	n = fread(text, 1, sizeof(text) - 1, f);
	text[n] = '\0';
	fclose(f);
	ASSERT_EQ(strstr(text, "unit 7 holding 0 = 100\nunit 7 holding 1 = 101\nunit 7 holding 1 = 5\n") != NULL);
	ASSERT_EQ(count_char(text, '\n') == 3);
	mogbus_destroy(ctx);
}
