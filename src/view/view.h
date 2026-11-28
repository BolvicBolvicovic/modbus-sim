/**
 * @file
 * @brief Live view of the point values of a set of devices: a terminal table or text lines.
 */
#ifndef VIEW_H
#define VIEW_H

#include "device/device.h"
#include "util/log.h"
#include "vlib/arena.h"
#include "vlib/c_types.h"
#include <stdbool.h>
#include <stdio.h>

typedef struct view_t view_t;

/** @brief Limits of what the view keeps and draws. */
enum
{
	VIEW_MAX_DEVICES = 16,		/**< devices shown (the limit of a context) */
	VIEW_LOG_LINES	= 6,		/**< log lines kept for the pane below the table */
	VIEW_LOG_WIDTH	= 160,		/**< bytes of one kept log line, with the NUL */
	VIEW_MAX_ROWS	= 200,		/**< taller terminals are drawn at this height */
	VIEW_MAX_COLS	= 400,		/**< wider terminals are drawn at this width */
	VIEW_FRAME_MAX	= 128 * 1024,	/**< bytes of one drawn table frame */
};

/**
 * @brief The view of one context.
 *
 * On a terminal (stdout is one, the format is not JSON) it draws one table, redrawn in place
 * with ANSI sequences on every interval, and keeps the log lines in a pane below it so they do
 * not corrupt the drawing; the kept lines go to stderr when the view closes. Otherwise it prints
 * the snapshot of every point, then only the points that changed, as text or JSON lines.
 */
struct view_t
{
	u32		interval_ms;	/**< 0 = not open */
	bool		table;		/**< terminal table (otherwise lines) */
	bool		json;		/**< lines as JSON objects */
	bool		snapshot;	/**< the next lines render prints every point */
	bool		hooked;		/**< the log sink is ours */
	u64		next_ms;	/**< monotonic time of the next render */
	u64		last_ms;	/**< monotonic time of the last render; 0 = none yet */
	u32		height;		/**< terminal lines of the frame drawn last */
	u32		pane;		/**< lines of the log pane in that frame, with its title; 0 = none */
	u32		log_count;	/**< kept log lines, at most VIEW_LOG_LINES */
	u32		log_next;	/**< ring slot the next log line goes to */
	char		log[VIEW_LOG_LINES][VIEW_LOG_WIDTH];
	char*		frame;		/**< table frame buffer, VIEW_FRAME_MAX bytes (table only) */
	u64		(*shown)[DEVICE_MAX_POINTS];	/**< hash of the last printed text of every point (lines only) */
};

/**
 * @brief Opens the view, or lowers the interval of an open one.
 *
 * A terminal table is drawn only when stdout is a terminal and the log format is not JSON; the
 * log format is read here.
 *
 * @return false when the arena has no room for the buffers.
 */
bool	view_open(view_t* view, arena_t* arena, u32 interval_ms);

/** @brief Milliseconds until the next render is due (0 when due); a large value when closed. */
u32	view_wait_ms(const view_t* view, u64 now_ms);

/** @brief Renders and writes to stdout when the interval has passed. */
void	view_update(view_t* view, device_t* const* devices, u32 count, u64 now_ms);

/** @brief Leaves the terminal as it was, writes the kept log lines to stderr, closes the view. */
void	view_close(view_t* view);

/**
 * @brief Renders the table frame for a terminal of the given size into @p out.
 *
 * Starts with the cursor movement that overwrites the previous frame and does not touch stdout.
 * Lines are cut to the width and the frame never exceeds the height, so what does not fit is
 * counted in a "N more" line.
 *
 * @return Bytes written, without a NUL.
 */
u32	view_render_table(view_t* view, device_t* const* devices, u32 count, u32 rows, u32 cols,
			  u64 now_ms, char* out, u32 size);

/**
 * @brief Renders what leaves the terminal as it was: the log pane erased (its lines go to stderr
 *        once, when the view closes) and the cursor shown again.
 *
 * @return Bytes written, without a NUL.
 */
u32	view_render_close(const view_t* view, char* out, u32 size);

/** @brief Prints every point (or the changed ones when !all) as text or JSON lines to @p out. */
void	view_render_lines(view_t* view, device_t* const* devices, u32 count, bool all, u64 now_ms,
			  FILE* out);

#endif
