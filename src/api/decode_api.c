/**
 * @file
 * @brief mogbus_decode_frame(): the public wrapper of the frame decoder (`src/decode/`).
 *
 * It touches no context, so it takes no lock and may run on any thread.
 */
#include "mogbus.h"
#include "decode/decode.h"

mogbus_result_t
mogbus_decode_frame(const uint8_t* data, size_t size, mogbus_framing_t framing,
		    mogbus_direction_t direction, bool json, char* out, size_t cap, size_t* len)
{
	u32		written = 0;
	decode_status_t	status;

	if (len)
		*len = 0;
	if (size > DECODE_MAX_FRAME ||
	    cap > 0xFFFFFFFFu ||
	    (unsigned)framing > MOGBUS_FRAMING_ASCII ||
	    (unsigned)direction > MOGBUS_DIRECTION_RESPONSE)
		return MOGBUS_ERR_INVALID;
	status = decode_frame(data, (u32)size, (decode_framing_t)framing, (decode_direction_t)direction,
			      json, out, (u32)cap, &written);
	if (len)
		*len = written;
	switch (status)
	{
		case DECODE_OK        : return MOGBUS_OK;
		case DECODE_MALFORMED : return MOGBUS_ERR_FRAME;
		case DECODE_TRUNCATED : return MOGBUS_ERR_NOMEM;
		default               : return MOGBUS_ERR_INVALID;
	}
}
