/**
 * @file
 * @brief Live view of the point values: the terminal table and the line output.
 */
#include "view/view.h"
#include "os/os_console.h"
#include "os/os_time.h"
#include <math.h>
#include <stdarg.h>
#include <string.h>

/** @brief Buffer sizes and the fixed lines of a table frame. */
enum
{
	VIEW_VALUE_MAX	= 2 * DEVICE_MAX_STRING_REGISTERS * 6 + 8,	/**< a quoted string with every byte escaped as six characters */
	VIEW_FIXED_ROWS	= 2,						/**< title and column header */
};

static const char* const SPACE_NAMES[SPACE_COUNT] = {
	[SPACE_COIL]     = "coil",
	[SPACE_DISCRETE] = "discrete",
	[SPACE_INPUT]    = "input",
	[SPACE_HOLDING]  = "holding",
};

static const char* const TYPE_NAMES[] = {
	[VT_U16] = "u16", [VT_S16] = "s16", [VT_U32] = "u32", [VT_S32] = "s32", [VT_F32] = "f32",
	[VT_BOOL] = "bool", [VT_U64] = "u64", [VT_S64] = "s64", [VT_F64] = "f64", [VT_STRING] = "string",
};

static const char* const LEVEL_NAMES[] = {
	[LOG_LEVEL_DEBUG] = "debug", [LOG_LEVEL_INFO] = "info", [LOG_LEVEL_WARN] = "warn",
};

/** @brief Keeps a log line for the pane; control characters would break the drawing. */
static void
view_log_sink(void* user, log_level_t level, const char* message)
{
	view_t*	view = user;
	char*	slot = view->log[view->log_next];

	snprintf(slot, VIEW_LOG_WIDTH, "[%s] %s", level < LOG_LEVEL_OFF ? LEVEL_NAMES[level] : "log", message);
	for (char* c = slot; *c; c++)
	{
		if ((unsigned char)*c < 0x20 ||
		    *c == 0x7F)
			*c = ' ';
	}
	view->log_next = (view->log_next + 1) % VIEW_LOG_LINES;
	if (view->log_count < VIEW_LOG_LINES)
		view->log_count++;
}

bool
view_open(view_t* view, arena_t* arena, u32 interval_ms)
{
	if (view->interval_ms)
	{
		if (interval_ms < view->interval_ms)
			view->interval_ms = interval_ms;
		return true;
	}
	memset(view, 0, sizeof(*view));
	view->json  = log_format() == LOG_FORMAT_JSON;
	view->table = !view->json &&
		      os_console_terminal(NULL, NULL);
	if (view->table)
		view->frame = ARENA_PUSH_ARRAY_ALIGNED(arena, char, VIEW_FRAME_MAX, 16);
	else
		view->shown = (u64 (*)[DEVICE_MAX_POINTS])ARENA_PUSH_ARRAY_ALIGNED(
			arena, u64, (u64)VIEW_MAX_DEVICES * DEVICE_MAX_POINTS, 16);
	if (!view->frame &&
	    !view->shown)
		return false;
	if (view->table &&
	    !log_get_sink())
	{
		log_set_sink(view_log_sink, view);
		view->hooked = true;
	}
	view->snapshot    = true;
	view->interval_ms = interval_ms;
	return true;
}

u32
view_wait_ms(const view_t* view, u64 now_ms)
{
	if (!view->interval_ms)
		return 0xFFFFFFFFu;
	if (view->next_ms <= now_ms)
		return 0;
	return view->next_ms - now_ms > 0xFFFFFFFEull ? 0xFFFFFFFEu : (u32)(view->next_ms - now_ms);
}

/**
 * @brief Formats the current value of a point: a number as the host would print it, a string
 *        quoted and escaped.
 */
static void
point_text(const device_t* dev, const point_t* point, bool json, char* value, u32 size)
{
	char	text[2 * DEVICE_MAX_STRING_REGISTERS + 1] = "";
	val_t	val = { 0 };
	f64	number;
	u32	used = 0;

	if (point->type != VT_STRING &&
	    device_get_point(dev, point->space, point->address, &val))
	{
		number = val.type == VAL_BOOL ? (val.b ? 1.0 : 0.0) : val.n;
		if (!isfinite(number))
			snprintf(value, size, json ? "null" : "%s", isnan(number) ? "nan" : number < 0 ? "-inf" : "inf");
		else
			snprintf(value, size, point->type == VT_F32 ? "%.7g" : point->type == VT_F64 ? "%.15g" : "%.0f",
				 number);
		return;
	}
	if (point->type == VT_STRING)
		(void)device_get_string(dev, point->space, point->address, text, sizeof(text));

	value[used++] = '"';
	for (const char* c = text; *c && used + 7 < size; c++)
	{
		unsigned char ch = (unsigned char)*c;

		if (ch == '"' ||
		    ch == '\\')
		{
			value[used++] = '\\';
			value[used++] = (char)ch;
		}
		else if (ch < 0x20 ||
			 ch >= 0x7F)
			used += (u32)snprintf(value + used, 7, "\\u%04x", ch);
		else
			value[used++] = (char)ch;
	}
	value[used++] = '"';
	value[used]   = '\0';
}

void
view_render_lines(view_t* view, device_t* const* devices, u32 count, bool all, u64 now_ms, FILE* out)
{
	char value[VIEW_VALUE_MAX];

	for (u32 d = 0; d < count && d < VIEW_MAX_DEVICES; d++)
	{
		device_t* dev = devices[d];

		device_tick(dev, now_ms);
		for (u32 i = 0; i < dev->point_count; i++)
		{
			const point_t*	point = &dev->points[i];
			u64		hash  = 14695981039346656037ull;

			point_text(dev, point, view->json, value, sizeof(value));
			for (const char* c = value; *c; c++)
				hash = (hash ^ (unsigned char)*c) * 1099511628211ull;
			if (!all &&
			    hash == view->shown[d][i])
				continue;
			view->shown[d][i] = hash;
			if (view->json)
				fprintf(out, "{\"unit\":%u,\"space\":\"%s\",\"address\":%u,\"value\":%s}\n",
					dev->unit_id, SPACE_NAMES[point->space], point->address, value);
			else
				fprintf(out, "unit %u %s %u = %s\n", dev->unit_id, SPACE_NAMES[point->space],
					point->address, value);
		}
	}
}

/** @brief Appends one line cut to the width, followed by erase-to-end-of-line and optionally a newline. */
static void __attribute__((format(printf, 6, 7)))
put_line(char* out, u32* len, u32 size, bool newline, u32 cols, const char* fmt, ...)
{
	char	line[VIEW_MAX_COLS + 2 * VIEW_VALUE_MAX];
	va_list	args;
	u32	n;

	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	n = (u32)strlen(line);
	if (n > cols - 1)
		n = cols - 1;
	if (*len + n + 5 >= size)
		return;
	memcpy(out + *len, line, n);
	*len += n;
	memcpy(out + *len, "\x1b[K", 3);
	*len += 3;
	if (newline)
		out[(*len)++] = '\n';
}

u32
view_render_table(view_t* view, device_t* const* devices, u32 count, u32 rows, u32 cols,
		  u64 now_ms, char* out, u32 size)
{
	char		value[VIEW_VALUE_MAX];
	char		achieved[48] = "-";
	os_civil_t	civil        = { 0 };
	u32		len          = 0;
	u32		total        = 0;
	u32		pane         = 0;
	u32		printed      = 0;
	u32		body;
	u32		shown;

	rows = rows > VIEW_MAX_ROWS ? VIEW_MAX_ROWS : rows < 4 ? 4 : rows;
	cols = cols > VIEW_MAX_COLS ? VIEW_MAX_COLS : cols < 20 ? 20 : cols;
	if (count > VIEW_MAX_DEVICES)
		count = VIEW_MAX_DEVICES;
	for (u32 d = 0; d < count; d++)
	{
		device_tick(devices[d], now_ms);
		total += devices[d]->point_count;
	}
	if (view->log_count &&
	    rows >= 12)
		pane = view->log_count < (rows - 6) / 3 ? view->log_count : (rows - 6) / 3;
	body  = rows - VIEW_FIXED_ROWS - (pane ? pane + 1 : 0);
	shown = total > body ? body - 1 : total;

	/* Back to the first line of the previous frame, which this one overwrites. */
	if (view->height > 1)
		len = (u32)snprintf(out, size, "\r\x1b[%uA", view->height - 1);
	else
		len = (u32)snprintf(out, size, "\r");

	if (view->last_ms &&
	    now_ms > view->last_ms)
		snprintf(achieved, sizeof(achieved), "%.2f/s", 1000.0 / (double)(now_ms - view->last_ms));
	(void)os_civil_from_unix(os_wall_ms() / 1000, false, &civil);
	put_line(out, &len, size, true, cols, "mogbus  %u device%s, %u point%s  refresh %u ms, achieved %s  %02u:%02u:%02u",
		 count, count == 1 ? "" : "s", total, total == 1 ? "" : "s", view->interval_ms, achieved,
		 civil.hour, civil.min, civil.sec);
	put_line(out, &len, size, true, cols, "%-4s  %-8s  %5s  %-6s  %s", "UNIT", "TABLE", "ADDR", "TYPE", "VALUE");
	view->height = VIEW_FIXED_ROWS;
	view->pane   = pane ? pane + 1 : 0;

	for (u32 d = 0; d < count && printed < shown; d++)
	{
		const device_t* dev = devices[d];

		for (u32 i = 0; i < dev->point_count && printed < shown; i++, printed++)
		{
			const point_t* point = &dev->points[i];

			point_text(dev, point, false, value, sizeof(value));
			put_line(out, &len, size, true, cols, "%-4u  %-8s  %5u  %-6s  %s", dev->unit_id,
				 SPACE_NAMES[point->space], point->address, TYPE_NAMES[point->type], value);
			view->height++;
		}
	}
	if (total > shown)
	{
		put_line(out, &len, size, true, cols, "... %u more point%s (the terminal is too small)", total - shown,
			 total - shown == 1 ? "" : "s");
		view->height++;
	}
	if (pane)
	{
		put_line(out, &len, size, true, cols, "-- log --");
		view->height++;
		for (u32 i = 0; i < pane; i++)
		{
			u32 slot = (view->log_next + VIEW_LOG_LINES - pane + i) % VIEW_LOG_LINES;

			put_line(out, &len, size, i + 1 < pane, cols, "%s", view->log[slot]);
			view->height++;
		}
	}
	else if (len > 0 &&
		 out[len - 1] == '\n')
		len--;	/* no newline after the last line: the cursor stays on it */
	if (len + 4 < size)
	{
		memcpy(out + len, "\x1b[J", 3);
		len += 3;
	}
	out[len] = '\0';
	return len;
}

u32
view_render_close(const view_t* view, char* out, u32 size)
{
	if (view->pane > 1)
		return (u32)snprintf(out, size, "\r\x1b[%uA\x1b[J\x1b[?25h", view->pane - 1);
	if (view->pane == 1)
		return (u32)snprintf(out, size, "\r\x1b[J\x1b[?25h");
	return (u32)snprintf(out, size, "\n\x1b[?25h");
}

void
view_update(view_t* view, device_t* const* devices, u32 count, u64 now_ms)
{
	if (!view->interval_ms ||
	    now_ms < view->next_ms)
		return;
	if (view->table)
	{
		u32 rows = 24;
		u32 cols = 80;
		u32 n;

		(void)os_console_terminal(&rows, &cols);
		n = view_render_table(view, devices, count, rows, cols, now_ms, view->frame, VIEW_FRAME_MAX);
		if (!view->last_ms)
			fputs("\x1b[?25l", stdout);
		fwrite(view->frame, 1, n, stdout);
	}
	else
	{
		view_render_lines(view, devices, count, view->snapshot, now_ms, stdout);
		view->snapshot = false;
	}
	fflush(stdout);
	view->last_ms = now_ms;
	view->next_ms = now_ms + view->interval_ms;
}

void
view_close(view_t* view)
{
	if (!view->interval_ms)
		return;
	if (view->table &&
	    view->height)
	{
		char out[32];

		view_render_close(view, out, sizeof(out));
		fputs(out, stdout);
		fflush(stdout);
	}
	if (view->hooked &&
	    log_get_sink() == view_log_sink)
		log_set_sink(NULL, NULL);
	for (u32 i = 0; i < view->log_count; i++)
		fprintf(stderr, "%s\n", view->log[(view->log_next + VIEW_LOG_LINES - view->log_count + i) % VIEW_LOG_LINES]);
	view->interval_ms = 0;
}
