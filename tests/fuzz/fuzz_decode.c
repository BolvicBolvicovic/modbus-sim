/**
 * @file
 * @brief Fuzzes the frame decoder: any bytes in any framing and direction, as text and JSON,
 *        into a full-size buffer and into a tiny one. It must never crash, never write past the
 *        buffer, always NUL-terminate, and a complete JSON output must be balanced.
 *
 * Input layout: [framing 0-3][direction 0-2][json bit, small buffer size][frame bytes].
 */
#include "decode/decode.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_CHECK(cond)	do { if (!(cond)) __builtin_trap(); } while (0)

/** @brief true when brackets and quotes of a JSON text close properly. */
static bool
json_balanced(const char* s, u32 len)
{
	u32	depth = 0;
	bool	quoted = false;

	for (u32 i = 0; i < len; i++)
	{
		char ch = s[i];

		if (quoted)
		{
			if (ch == '\\')
				i++;
			else if (ch == '"')
				quoted = false;
			continue;
		}
		if (ch == '"')
			quoted = true;
		else if (ch == '{' || ch == '[')
			depth++;
		else if (ch == '}' || ch == ']')
		{
			if (depth == 0)
				return false;
			depth--;
		}
	}
	return depth == 0 && !quoted;
}

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	static char		big[65536];
	char			small[80 + 32];
	decode_framing_t	framing;
	decode_direction_t	direction;
	bool			json;
	u32			len = 0;
	u32			small_cap;
	decode_status_t		status;

	if (size < 3)
		return 0;
	framing   = (decode_framing_t)(data[0] % 4);
	direction = (decode_direction_t)(data[1] % 3);
	json      = (data[2] & 1) != 0;
	small_cap = (u32)(data[2] >> 1) % 80 + 1;
	data += 3;
	size -= 3;

	status = decode_frame(data, (u32)size, framing, direction, json, big, sizeof(big), &len);
	if (status == DECODE_INVALID)
		return 0;
	FUZZ_CHECK(len < sizeof(big));
	FUZZ_CHECK(big[len] == '\0');
	FUZZ_CHECK(strlen(big) == len);
	FUZZ_CHECK(status != DECODE_TRUNCATED);
	if (json)
		FUZZ_CHECK(json_balanced(big, len));

	memset(small, 0xA5, sizeof(small));
	status = decode_frame(data, (u32)size, framing, direction, json, small, small_cap, &len);
	FUZZ_CHECK(len < small_cap);
	FUZZ_CHECK(small[len] == '\0');
	for (u32 i = small_cap; i < sizeof(small); i++)
		FUZZ_CHECK((u8)small[i] == 0xA5);
	if (status != DECODE_TRUNCATED)
		FUZZ_CHECK(status == DECODE_OK || status == DECODE_MALFORMED);
	return 0;
}
