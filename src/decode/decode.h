/**
 * @file
 * @brief Frame decoder: a captured MBAP, RTU or ASCII frame as readable text or one JSON object.
 */
#ifndef DECODE_H
#define DECODE_H

#include "vlib/c_types.h"
#include <stdbool.h>

typedef enum decode_framing_t decode_framing_t;
typedef enum decode_direction_t decode_direction_t;
typedef enum decode_status_t decode_status_t;

/** @brief The largest frame decode_frame() accepts, in bytes. */
#define DECODE_MAX_FRAME	4096

/** @brief Which ADU format the bytes are in. */
enum decode_framing_t
{
	DECODE_FRAMING_AUTO,	/**< detect it from the bytes */
	DECODE_FRAMING_MBAP,
	DECODE_FRAMING_RTU,
	DECODE_FRAMING_ASCII,
};

/** @brief Whether the PDU is a request or a response. */
enum decode_direction_t
{
	DECODE_DIRECTION_AUTO,	/**< infer it from the length and the byte count */
	DECODE_DIRECTION_REQUEST,
	DECODE_DIRECTION_RESPONSE,
};

/** @brief Outcome of decode_frame(). */
enum decode_status_t
{
	DECODE_OK,		/**< decoded; no structural problem */
	DECODE_MALFORMED,	/**< decoded as far as possible; the text lists what is wrong */
	DECODE_TRUNCATED,	/**< the output buffer was too small; the text is cut */
	DECODE_INVALID,		/**< NULL arguments, no output room, or a frame above DECODE_MAX_FRAME */
};

/**
 * @brief Decodes one frame into `out` as text or as one compact JSON object (no trailing
 *        newline).
 *
 * Nothing is allocated and the output is always NUL-terminated within `cap`. `*len` receives the
 * text length, without the NUL. A frame that is damaged (bad checksum, truncated, wrong length)
 * still prints every field that could be read, then lists the problems.
 */
decode_status_t	decode_frame(const u8* data, u32 size, decode_framing_t framing,
			     decode_direction_t direction, bool json, char* out, u32 cap, u32* len);

#endif
