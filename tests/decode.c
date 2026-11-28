/* The frame decoder: every function code in both directions and all three framings, damaged
 * frames, truncated frames, ambiguous detection, output limits and the public wrapper. */
#include "vtest.h"
#include "decode/decode.h"
#include "modbus/ascii.h"
#include "modbus/mbap.h"
#include "modbus/rtu.h"
#include "mogbus.h"
#include "vlib/macros.h"
#include <stdio.h>
#include <string.h>

/* ---- helpers ---- */

static u32
from_hex(const char* text, u8* out)
{
	u32	n = 0;
	int	high = -1;

	for (; *text; text++)
	{
		int v;

		if (*text == ' ')
			continue;
		v = *text <= '9' ? *text - '0' : (*text | 0x20) - 'a' + 10;
		if (high < 0)
			high = v;
		else
		{
			out[n++] = (u8)((high << 4) | v);
			high     = -1;
		}
	}
	return n;
}

/* Wraps a PDU for the unit 1 in the framing (never DECODE_FRAMING_AUTO). */
static u32
wrap(decode_framing_t framing, const u8* pdu, u32 len, u8* out)
{
	if (framing == DECODE_FRAMING_MBAP)
	{
		mbap_header_t header = { .transaction = 0x1234, .protocol = 0, .length = (u16)(len + 1), .unit = 1 };

		mbap_encode(out, &header);
		memcpy(out + MBAP_HEADER_SIZE, pdu, len);
		return MBAP_HEADER_SIZE + len;
	}
	if (framing == DECODE_FRAMING_RTU)
		return modbus_rtu_encode(out, 1, pdu, len);
	return modbus_ascii_encode(out, 1, pdu, len);
}

static const char*
framing_text(decode_framing_t framing)
{
	return framing == DECODE_FRAMING_MBAP ? "mbap" : framing == DECODE_FRAMING_RTU ? "rtu" : "ascii";
}

/* A small JSON syntax checker: returns the end of the value at p, or NULL. */
static const char*
json_value(const char* p)
{
	while (*p == ' ')
		p++;
	if (*p == '{' ||
	    *p == '[')
	{
		char close = *p == '{' ? '}' : ']';

		p++;
		if (*p == close)
			return p + 1;
		for (;;)
		{
			if (close == '}')
			{
				p = json_value(p);
				if (!p ||
				    *p != ':')
					return NULL;
				p++;
			}
			p = json_value(p);
			if (!p)
				return NULL;
			if (*p == ',')
			{
				p++;
				continue;
			}
			return *p == close ? p + 1 : NULL;
		}
	}
	if (*p == '"')
	{
		for (p++; *p && *p != '"'; p++)
		{
			if ((u8)*p < 0x20)
				return NULL;
			if (*p == '\\')
				p++;
		}
		return *p == '"' ? p + 1 : NULL;
	}
	if (strncmp(p, "true", 4) == 0)
		return p + 4;
	if (strncmp(p, "false", 5) == 0)
		return p + 5;
	if (strncmp(p, "null", 4) == 0)
		return p + 4;
	if (*p == '-' ||
	    (*p >= '0' && *p <= '9'))
	{
		const char* start = p;

		p += *p == '-';
		while (*p >= '0' && *p <= '9')
			p++;
		return p > start ? p : NULL;
	}
	return NULL;
}

static bool
json_valid(const char* text)
{
	const char* end = json_value(text);

	return end && *end == '\0';
}

static char g_text[MOGBUS_DECODE_BUFFER_SIZE];
static char g_json[MOGBUS_DECODE_BUFFER_SIZE];

static decode_status_t
run(const u8* frame, u32 len, decode_framing_t framing, decode_direction_t direction, bool json)
{
	char*	out = json ? g_json : g_text;
	u32	out_len = 0;
	decode_status_t	status = decode_frame(frame, len, framing, direction, json, out, sizeof(g_text), &out_len);

	if (status == DECODE_INVALID)
		return status;
	if (strlen(out) != out_len)
		return DECODE_INVALID;
	return status;
}

/* ---- every function code ---- */

typedef struct vector_t vector_t;

struct vector_t
{
	const char*	pdu;
	const char*	direction;	/* what auto picks */
	const char*	want[5];	/* substrings of the text */
};

static const vector_t vectors[] = {
	{ "01 00 13 00 25", "request", { "function: 1 (0x01) Read Coils", "address: 19 (0x0013)", "quantity: 37" } },
	{ "01 05 CD 6B B2 0E 1B", "response", { "byte_count: 5", "bits: 1 0 1 1 0 0 1 1 1 1 0 1 0 1 1 0", "data: CD 6B B2 0E 1B" } },
	{ "02 00 C4 00 16", "request", { "Read Discrete Inputs", "address: 196 (0x00C4)", "quantity: 22" } },
	{ "02 03 AC DB 35", "response", { "byte_count: 3", "bits: 0 0 1 1 0 1 0 1" } },
	{ "03 00 6B 00 03", "request", { "Read Holding Registers", "address: 107 (0x006B)", "quantity: 3" } },
	{ "03 06 02 2B 00 00 00 64", "response", { "byte_count: 6", "values: 555 0 100", "[hex 022B 0000 0064]" } },
	{ "04 00 08 00 01", "request", { "Read Input Registers", "address: 8 (0x0008)", "quantity: 1" } },
	{ "04 02 00 0A", "response", { "values: 10" } },
	{ "05 00 AC FF 00", "request", { "Write Single Coil", "address: 172 (0x00AC)", "value: 65280 (0xFF00) ON" } },
	{ "06 00 01 00 03", "request", { "Write Single Register", "address: 1 (0x0001)", "value: 3 (0x0003)" } },
	{ "07", "request", { "function: 7 (0x07) Read Exception Status" } },
	{ "07 6D", "response", { "Read Exception Status", "output_data: 109 (0x6D)" } },
	{ "08 00 00 A5 37", "request", { "Diagnostics", "sub_function_text: Return Query Data", "data: 42295 (0xA537)" } },
	{ "08 00 0A 00 00", "request", { "sub_function: 10 (0x000A)", "Clear Counters and Diagnostic Register" } },
	{ "08 00 01 FF 00", "request", { "Restart Communications Option", "clear the event log" } },
	{ "0B", "request", { "Get Comm Event Counter" } },
	{ "0B FF FF 01 08", "response", { "status: 65535 (0xFFFF) busy", "event_count: 264" } },
	{ "0C", "request", { "Get Comm Event Log" } },
	{ "0C 08 00 00 01 08 01 21 80 00", "response",
	  { "byte_count: 8", "status: 0 (0x0000) ready", "message_count: 289", "event: 128 (0x80) receive event",
	    "event: 0 (0x00) communications restart" } },
	{ "0F 00 13 00 0A 02 CD 01", "request",
	  { "Write Multiple Coils", "quantity: 10", "byte_count: 2", "coils: 1 0 1 1 0 0 1 1 1 0" } },
	{ "0F 00 13 00 0A", "response", { "Write Multiple Coils", "address: 19 (0x0013)", "quantity: 10" } },
	{ "10 00 01 00 02 04 00 0A 01 02", "request", { "Write Multiple Registers", "byte_count: 4", "values: 10 258" } },
	{ "10 00 01 00 02", "response", { "address: 1 (0x0001)", "quantity: 2" } },
	{ "11", "request", { "Report Server ID" } },
	{ "11 04 11 FF 01 02", "response",
	  { "server_id: 17 (0x11)", "run_indicator: 255 (0xFF) ON", "additional_data: 01 02" } },
	{ "14 0E 06 00 04 00 01 00 02 06 00 03 00 09 00 02", "request",
	  { "Read File Record", "sub_requests:", "file_number: 4", "file_number: 3", "record_number: 9" } },
	{ "14 0C 05 06 0D FE 00 20 05 06 33 CD 00 40", "response",
	  { "data_length: 12", "sub_responses:", "data: 3582 32", "data: 13261 64" } },
	{ "15 0D 06 00 04 00 07 00 03 06 AF 04 BE 10 0D", "request",
	  { "Write File Record", "file_number: 4", "record_length: 3", "data: 1711 1214 4109" } },
	{ "16 00 04 00 F2 00 25", "request",
	  { "Mask Write Register", "and_mask: 242 (0x00F2)", "or_mask: 37 (0x0025)" } },
	{ "17 00 03 00 06 00 0E 00 03 06 00 FF 00 FF 00 FF", "request",
	  { "Read/Write Multiple Registers", "read_quantity: 6", "write_address: 14 (0x000E)", "write_quantity: 3",
	    "values: 255 255 255" } },
	{ "17 0C 00 FE 0A CD 00 01 00 03 00 0D 00 FF", "response", { "byte_count: 12", "values: 254 2765 1 3 13 255" } },
	{ "18 04 DE", "request", { "Read FIFO Queue", "fifo_address: 1246 (0x04DE)" } },
	{ "18 00 06 00 02 01 B8 12 84", "response", { "byte_count: 6", "fifo_count: 2", "values: 440 4740" } },
	{ "2B 0E 01 00", "request",
	  { "Encapsulated Interface Transport", "mei_type: 14 (0x0E) Read Device Identification",
	    "read_device_id_code: 1 (0x01) basic stream", "object_id: 0 (0x00)", "object_name: VendorName" } },
	{ "2B 0E 01 01 00 00 02 00 04 41 63 6D 65 01 03 31 2E 30", "response",
	  { "conformity_level: 1 (0x01) basic, stream", "more_follows: 0 (0x00)", "number_of_objects: 2",
	    "id: 0 (0x00) VendorName", "value: Acme" } },
	{ "2B 0D 00 00 05 60 41 00 00 00 00 04", "request",
	  { "CANopen General Reference", "access: read (upload)", "node: 5", "index: 24641 (0x6041)", "count: 4" } },
	{ "2B 0D 00 00 05 60 41 00 00 00 00 04 17 02 00 00", "response",
	  { "access: read (upload)", "count: 4", "data: 17 02 00 00" } },
	{ "2B 0D 01 00 05 60 41 00 00 00 00 02 AA BB", "request",
	  { "access: write (download)", "count: 2", "data: AA BB" } },
	{ "AB FF 00 06 0D CE 0B 00 FF FF", "response",
	  { "failure: 206 (0xCE) service failure", "error_code: 4294901771 (0xFFFF000B) object does not exist" } },
	{ "AB FF 00 03 0D AE 05", "response", { "failure: 174 (0xAE) unsupported option", "supported_control: 5 (0x05)" } },
	{ "83 02", "response", { "function: 3 (0x03) Read Holding Registers", "exception: 2 (0x02) Illegal Data Address" } },
	{ "91 04", "response", { "Report Server ID", "exception: 4 (0x04) Slave Device Failure" } },
	{ "8B 0B", "response", { "exception: 11 (0x0B) Gateway Target Device Failed to Respond" } },
	{ "41 01 02", "unknown", { "function: 65 (0x41) user-defined", "data: 01 02" } },
	{ "64", "unknown", { "function: 100 (0x64) user-defined" } },
	{ "09 01", "unknown", { "function: 9 (0x09) unknown" } },
};

static const vector_t forced_responses[] = {
	{ "05 00 AC FF 00", "response", { "direction: response", "value: 65280 (0xFF00)" } },
	{ "06 00 01 00 03", "response", { "direction: response", "value: 3 (0x0003)" } },
	{ "08 00 0B 00 08", "response", { "Return Bus Message Count", "data: 8 (0x0008)" } },
	{ "16 00 04 00 F2 00 25", "response", { "direction: response", "or_mask: 37 (0x0025)" } },
	{ "15 0D 06 00 04 00 07 00 03 06 AF 04 BE 10 0D", "response", { "sub_responses:", "data: 1711 1214 4109" } },
	{ "01 03 CD 6B 05", "response", { "byte_count: 3", "bits: 1 0 1 1 0 0 1 1" } },
};

static const decode_framing_t framings[] = { DECODE_FRAMING_MBAP, DECODE_FRAMING_RTU, DECODE_FRAMING_ASCII };

static void
check_vector(const vector_t* v, decode_framing_t framing, decode_framing_t as, decode_direction_t dir,
	     const char* direction)
{
	u8	pdu[MODBUS_MAX_PDU];
	u8	frame[MODBUS_ASCII_MAX_FRAME];
	u32	pdu_len = from_hex(v->pdu, pdu);
	u32	len     = wrap(framing, pdu, pdu_len, frame);
	char	want[64];

	ASSERT_EQ(len > 0);
	ASSERT_EQ(run(frame, len, as, dir, false) == DECODE_OK);
	snprintf(want, sizeof(want), "framing: %s\n", framing_text(framing));
	ASSERT_EQ(strstr(g_text, want) != NULL);
	snprintf(want, sizeof(want), "direction: %s\n", direction);
	ASSERT_EQ(strstr(g_text, want) != NULL);
	ASSERT_EQ(strstr(g_text, "ok: true\n") != NULL);
	for (u32 k = 0; k < ARRAY_COUNT(v->want) && v->want[k]; k++)
	{
		if (!strstr(g_text, v->want[k]))
			printf("pdu %s as %s: missing '%s' in:\n%s\n", v->pdu, framing_text(framing), v->want[k], g_text);
		ASSERT_EQ(strstr(g_text, v->want[k]) != NULL);
	}
	ASSERT_EQ(run(frame, len, as, dir, true) == DECODE_OK);
	if (!json_valid(g_json))
		printf("invalid JSON: %s\n", g_json);
	ASSERT_EQ(json_valid(g_json));
	snprintf(want, sizeof(want), "\"direction\":\"%s\"", direction);
	ASSERT_EQ(strstr(g_json, want) != NULL);
	ASSERT_EQ(strstr(g_json, "\"ok\":true") != NULL);
	ASSERT_EQ(g_json[0] == '{');
}

TEST(DecodeEveryFunctionCodeAutoDetectsFramingAndDirection)
{
	for (u32 i = 0; i < ARRAY_COUNT(vectors); i++)
	{
		for (u32 f = 0; f < ARRAY_COUNT(framings); f++)
			check_vector(&vectors[i], framings[f], DECODE_FRAMING_AUTO, DECODE_DIRECTION_AUTO,
				     vectors[i].direction);
	}
}

TEST(DecodeEveryFunctionCodeWithForcedFramingAndDirection)
{
	for (u32 i = 0; i < ARRAY_COUNT(vectors); i++)
	{
		decode_direction_t dir = strcmp(vectors[i].direction, "response") == 0 ? DECODE_DIRECTION_RESPONSE :
					 DECODE_DIRECTION_REQUEST;

		if (strcmp(vectors[i].direction, "unknown") == 0)
			continue;
		for (u32 f = 0; f < ARRAY_COUNT(framings); f++)
			check_vector(&vectors[i], framings[f], framings[f], dir, vectors[i].direction);
	}
}

TEST(DecodeForcedResponseOfAmbiguousPdus)
{
	for (u32 i = 0; i < ARRAY_COUNT(forced_responses); i++)
		check_vector(&forced_responses[i], DECODE_FRAMING_RTU, DECODE_FRAMING_RTU, DECODE_DIRECTION_RESPONSE,
			     "response");
}

TEST(DecodeAutoDirectionExplainsItself)
{
	u8	pdu[MODBUS_MAX_PDU];
	u8	frame[MODBUS_RTU_MAX_FRAME];
	u32	pdu_len = from_hex("05 00 AC FF 00", pdu);
	u32	len     = wrap(DECODE_FRAMING_RTU, pdu, pdu_len, frame);

	ASSERT_EQ(run(frame, len, DECODE_FRAMING_RTU, DECODE_DIRECTION_AUTO, false) == DECODE_OK);
	ASSERT_EQ(strstr(g_text, "direction_reason: length and fields fit both a request and a response; assumed request") != NULL);
	pdu_len = from_hex("03 06 02 2B 00 00 00 64", pdu);
	len     = wrap(DECODE_FRAMING_RTU, pdu, pdu_len, frame);
	ASSERT_EQ(run(frame, len, DECODE_FRAMING_RTU, DECODE_DIRECTION_AUTO, false) == DECODE_OK);
	ASSERT_EQ(strstr(g_text, "direction_reason: length and byte count fit the response layout") != NULL);
	ASSERT_EQ(run(frame, len, DECODE_FRAMING_RTU, DECODE_DIRECTION_RESPONSE, false) == DECODE_OK);
	ASSERT_EQ(strstr(g_text, "direction_reason") == NULL);
}

TEST(DecodeFramingDetectionReasons)
{
	u8	pdu[MODBUS_MAX_PDU];
	u8	frame[MODBUS_ASCII_MAX_FRAME];
	u32	pdu_len = from_hex("03 00 6B 00 03", pdu);
	u32	len;

	len = wrap(DECODE_FRAMING_MBAP, pdu, pdu_len, frame);
	ASSERT_EQ(run(frame, len, DECODE_FRAMING_AUTO, DECODE_DIRECTION_AUTO, false) == DECODE_OK);
	ASSERT_EQ(strstr(g_text, "framing_reason: protocol id is 0 and the length field matches") != NULL);
	ASSERT_EQ(strstr(g_text, "transaction: 4660 (0x1234)") != NULL);
	ASSERT_EQ(strstr(g_text, "length: 6\n") != NULL);
	ASSERT_EQ(strstr(g_text, "unit: 1\n") != NULL);
	len = wrap(DECODE_FRAMING_RTU, pdu, pdu_len, frame);
	ASSERT_EQ(run(frame, len, DECODE_FRAMING_AUTO, DECODE_DIRECTION_AUTO, false) == DECODE_OK);
	ASSERT_EQ(strstr(g_text, "framing_reason: the CRC matches") != NULL);
	ASSERT_EQ(strstr(g_text, "crc_ok: true") != NULL);
	len = wrap(DECODE_FRAMING_ASCII, pdu, pdu_len, frame);
	ASSERT_EQ(run(frame, len, DECODE_FRAMING_AUTO, DECODE_DIRECTION_AUTO, false) == DECODE_OK);
	ASSERT_EQ(strstr(g_text, "framing_reason: starts with ':' and the LRC matches") != NULL);
	ASSERT_EQ(strstr(g_text, "lrc_ok: true") != NULL);
	/* lowercase hex is accepted, as on the wire */
	for (u32 i = 0; i < len; i++)
		frame[i] = (u8)(frame[i] >= 'A' && frame[i] <= 'F' ? frame[i] | 0x20 : frame[i]);
	ASSERT_EQ(run(frame, len, DECODE_FRAMING_AUTO, DECODE_DIRECTION_AUTO, false) == DECODE_OK);
}

TEST(DecodeAmbiguousFramingListsEveryCandidate)
{
	/* An MBAP frame whose last two bytes are the CRC of the rest is valid as RTU too. */
	u8	frame[12] = { 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x01, 0x03, 0x00, 0x00, 0, 0 };
	u16	crc       = modbus_crc16(frame, 10);

	frame[10] = (u8)(crc & 0xFF);
	frame[11] = (u8)(crc >> 8);
	ASSERT_EQ(run(frame, 12, DECODE_FRAMING_AUTO, DECODE_DIRECTION_AUTO, false) == DECODE_OK);
	ASSERT_EQ(strstr(g_text, "ambiguous: true") != NULL);
	ASSERT_EQ(strstr(g_text, "candidates:") != NULL);
	ASSERT_EQ(strstr(g_text, "framing: mbap") != NULL);
	ASSERT_EQ(strstr(g_text, "framing: rtu") != NULL);
	ASSERT_EQ(run(frame, 12, DECODE_FRAMING_AUTO, DECODE_DIRECTION_AUTO, true) == DECODE_OK);
	ASSERT_EQ(json_valid(g_json));
	ASSERT_EQ(strstr(g_json, "\"ambiguous\":true") != NULL);
	ASSERT_EQ(strstr(g_json, "\"candidates\":[{") != NULL);
	/* Forcing a framing decodes just that one. */
	ASSERT_EQ(run(frame, 12, DECODE_FRAMING_RTU, DECODE_DIRECTION_AUTO, false) == DECODE_MALFORMED);
	ASSERT_EQ(strstr(g_text, "ambiguous") == NULL);
	ASSERT_EQ(run(frame, 12, DECODE_FRAMING_MBAP, DECODE_DIRECTION_AUTO, false) == DECODE_OK);
	ASSERT_EQ(strstr(g_text, "ambiguous") == NULL);
}

/* ---- damaged frames ---- */

TEST(DecodeBadChecksumsAreReportedButStillDecoded)
{
	u8	pdu[MODBUS_MAX_PDU];
	u8	frame[MODBUS_ASCII_MAX_FRAME];
	u32	pdu_len = from_hex("03 00 6B 00 03", pdu);
	u32	len     = wrap(DECODE_FRAMING_RTU, pdu, pdu_len, frame);

	frame[len - 1] ^= 0x01;
	ASSERT_EQ(run(frame, len, DECODE_FRAMING_AUTO, DECODE_DIRECTION_AUTO, false) == DECODE_MALFORMED);
	ASSERT_EQ(strstr(g_text, "framing: rtu") != NULL);
	ASSERT_EQ(strstr(g_text, "crc_ok: false") != NULL);
	ASSERT_EQ(strstr(g_text, "crc_expected:") != NULL);
	ASSERT_EQ(strstr(g_text, "errors:\n  - CRC mismatch") != NULL);
	ASSERT_EQ(strstr(g_text, "quantity: 3") != NULL);
	ASSERT_EQ(strstr(g_text, "ok: false") != NULL);
	ASSERT_EQ(run(frame, len, DECODE_FRAMING_AUTO, DECODE_DIRECTION_AUTO, true) == DECODE_MALFORMED);
	ASSERT_EQ(json_valid(g_json));
	ASSERT_EQ(strstr(g_json, "\"crc_ok\":false") != NULL);
	ASSERT_EQ(strstr(g_json, "\"errors\":[\"CRC mismatch") != NULL);

	len = wrap(DECODE_FRAMING_ASCII, pdu, pdu_len, frame);
	frame[len - 3] = frame[len - 3] == '0' ? '1' : '0';
	ASSERT_EQ(run(frame, len, DECODE_FRAMING_AUTO, DECODE_DIRECTION_AUTO, false) == DECODE_MALFORMED);
	ASSERT_EQ(strstr(g_text, "framing: ascii") != NULL);
	ASSERT_EQ(strstr(g_text, "lrc_ok: false") != NULL);
	ASSERT_EQ(strstr(g_text, "LRC mismatch") != NULL);
	ASSERT_EQ(strstr(g_text, "lrc_expected:") != NULL);
}

TEST(DecodeMalformedAsciiFrames)
{
	static const struct
	{
		const char*	frame;
		const char*	problem;
	} cases[] = {
		{ ":010300000001FB", "does not end with CR LF" },
		{ ":0103000000G1FB\r\n", "invalid hex digit at character 11" },
		{ ":010300000001F\r\n", "odd number of hex digits" },
		{ ":0103\r\n", "needs an address, a function code and an LRC" },
		{ "010300000001FB\r\n", "starts with ':'" },
	};

	for (u32 i = 0; i < ARRAY_COUNT(cases); i++)
	{
		const u8* frame = (const u8*)cases[i].frame;

		ASSERT_EQ(run(frame, (u32)strlen(cases[i].frame), DECODE_FRAMING_ASCII, DECODE_DIRECTION_AUTO, false) ==
			  DECODE_MALFORMED);
		if (!strstr(g_text, cases[i].problem))
			printf("missing '%s' in:\n%s\n", cases[i].problem, g_text);
		ASSERT_EQ(strstr(g_text, cases[i].problem) != NULL);
		ASSERT_EQ(strstr(g_text, "ok: false") != NULL);
		ASSERT_EQ(run(frame, (u32)strlen(cases[i].frame), DECODE_FRAMING_ASCII, DECODE_DIRECTION_AUTO, true) ==
			  DECODE_MALFORMED);
		ASSERT_EQ(json_valid(g_json));
	}
	/* An unterminated frame is still decoded. */
	ASSERT_EQ(run((const u8*)":010300000001FB", 15, DECODE_FRAMING_ASCII, DECODE_DIRECTION_AUTO, false) ==
		  DECODE_MALFORMED);
	ASSERT_EQ(strstr(g_text, "quantity: 1") != NULL);
}

TEST(DecodeMalformedMbapHeaders)
{
	static const u8	short_length[] = { 0, 1, 0, 0, 0, 2, 1, 3, 0, 0, 0, 1 };
	static const u8	long_length[]  = { 0, 1, 0, 0, 0, 9, 1, 3, 0, 0, 0, 1 };
	static const u8	bad_protocol[] = { 0, 1, 0, 7, 0, 6, 1, 3, 0, 0, 0, 1 };
	static const u8	header_only[]  = { 0, 1, 0, 0, 0, 1, 1 };
	static const u8	cut_header[]   = { 0, 1, 0 };

	ASSERT_EQ(run(short_length, sizeof(short_length), DECODE_FRAMING_MBAP, DECODE_DIRECTION_AUTO, false) ==
		  DECODE_MALFORMED);
	ASSERT_EQ(strstr(g_text, "the length field says 2 bytes follow it, the frame has 6") != NULL);
	ASSERT_EQ(run(long_length, sizeof(long_length), DECODE_FRAMING_MBAP, DECODE_DIRECTION_AUTO, false) ==
		  DECODE_MALFORMED);
	ASSERT_EQ(strstr(g_text, "the length field says 9 bytes follow it, the frame has 6") != NULL);
	ASSERT_EQ(strstr(g_text, "quantity: 1") != NULL);
	ASSERT_EQ(run(bad_protocol, sizeof(bad_protocol), DECODE_FRAMING_MBAP, DECODE_DIRECTION_AUTO, false) ==
		  DECODE_MALFORMED);
	ASSERT_EQ(strstr(g_text, "protocol id is 0x0007, Modbus uses 0") != NULL);
	ASSERT_EQ(run(header_only, sizeof(header_only), DECODE_FRAMING_MBAP, DECODE_DIRECTION_AUTO, false) ==
		  DECODE_MALFORMED);
	ASSERT_EQ(strstr(g_text, "empty PDU") != NULL);
	ASSERT_EQ(run(cut_header, sizeof(cut_header), DECODE_FRAMING_MBAP, DECODE_DIRECTION_AUTO, false) ==
		  DECODE_MALFORMED);
	ASSERT_EQ(strstr(g_text, "truncated at byte 2: protocol needs 2 bytes, 1 left") != NULL);
	ASSERT_EQ(run(cut_header, 0, DECODE_FRAMING_AUTO, DECODE_DIRECTION_AUTO, false) == DECODE_MALFORMED);
	ASSERT_EQ(run(cut_header, 0, DECODE_FRAMING_AUTO, DECODE_DIRECTION_AUTO, true) == DECODE_MALFORMED);
	ASSERT_EQ(json_valid(g_json));
}

TEST(DecodeTruncatedPdusSayWhereTheyBreak)
{
	u8	pdu[MODBUS_MAX_PDU];
	u8	frame[MODBUS_RTU_MAX_FRAME];
	u32	pdu_len;
	u32	len;

	pdu_len = from_hex("03 00 00 00", pdu);
	len     = wrap(DECODE_FRAMING_RTU, pdu, pdu_len, frame);
	ASSERT_EQ(run(frame, len, DECODE_FRAMING_RTU, DECODE_DIRECTION_REQUEST, false) == DECODE_MALFORMED);
	ASSERT_EQ(strstr(g_text, "address: 0 (0x0000)") != NULL);
	ASSERT_EQ(strstr(g_text, "truncated at byte 4: quantity needs 2 bytes, 1 left") != NULL);

	pdu_len = from_hex("10 00 01 00 02 04 00 0A", pdu);
	len     = wrap(DECODE_FRAMING_MBAP, pdu, pdu_len, frame);
	ASSERT_EQ(run(frame, len, DECODE_FRAMING_MBAP, DECODE_DIRECTION_REQUEST, false) == DECODE_MALFORMED);
	ASSERT_EQ(strstr(g_text, "byte_count: 4") != NULL);
	ASSERT_EQ(strstr(g_text, "values: 10\n") != NULL || strstr(g_text, "values: 10  [hex 000A]") != NULL);
	ASSERT_EQ(strstr(g_text, "truncated at byte 13: the register data announces 4 bytes, 2 left") != NULL);
}

/* Found by fuzz_decode: an MBAP request ending right after 2B 0E read the device-id code byte past the end. */
TEST(DecodeDeviceIdRequestCutAfterMeiType)
{
	static const u8	frame[] = { 0x21, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x02, 0x02, 0x61, 0x01, 0x2B, 0x0E };
	u8		copy[sizeof(frame)];

	/* An exact-size copy so ASan catches a read one byte past the frame. */
	memcpy(copy, frame, sizeof(copy));
	ASSERT_EQ(run(copy, sizeof(copy), DECODE_FRAMING_AUTO, DECODE_DIRECTION_AUTO, false) != DECODE_INVALID);
	ASSERT_EQ(run(copy, sizeof(copy), DECODE_FRAMING_AUTO, DECODE_DIRECTION_AUTO, true) != DECODE_INVALID);
	ASSERT_EQ(json_valid(g_json));

	copy[5] = 0x03;	/* MBAP length that matches: unit, 2B 0E, a request cut after the MEI type */
	memcpy(copy + 7, (const u8[]){ 0x2B, 0x0E }, 2);
	ASSERT_EQ(run(copy, 9, DECODE_FRAMING_MBAP, DECODE_DIRECTION_REQUEST, false) == DECODE_MALFORMED);
}

TEST(DecodeUnexpectedTrailingBytes)
{
	u8	pdu[MODBUS_MAX_PDU];
	u8	frame[MODBUS_RTU_MAX_FRAME];
	u32	pdu_len = from_hex("03 00 00 00 01 EE FF", pdu);
	u32	len     = wrap(DECODE_FRAMING_RTU, pdu, pdu_len, frame);

	ASSERT_EQ(run(frame, len, DECODE_FRAMING_RTU, DECODE_DIRECTION_REQUEST, false) == DECODE_MALFORMED);
	ASSERT_EQ(strstr(g_text, "2 unexpected trailing bytes at byte 6 after the request") != NULL);
	ASSERT_EQ(strstr(g_text, "extra: EE FF") != NULL);
}

TEST(DecodeValuesAServerWouldRefuseAreWarningsNotErrors)
{
	u8	pdu[MODBUS_MAX_PDU];
	u8	frame[MODBUS_RTU_MAX_FRAME];
	u32	pdu_len = from_hex("03 00 00 00 00", pdu);
	u32	len     = wrap(DECODE_FRAMING_RTU, pdu, pdu_len, frame);

	ASSERT_EQ(run(frame, len, DECODE_FRAMING_RTU, DECODE_DIRECTION_REQUEST, false) == DECODE_OK);
	ASSERT_EQ(strstr(g_text, "warnings:\n  - quantity 0 is outside 1..125") != NULL);
	ASSERT_EQ(strstr(g_text, "ok: true") != NULL);
	ASSERT_EQ(run(frame, len, DECODE_FRAMING_RTU, DECODE_DIRECTION_REQUEST, true) == DECODE_OK);
	ASSERT_EQ(json_valid(g_json));
	ASSERT_EQ(strstr(g_json, "\"warnings\":[\"quantity 0") != NULL);
}

TEST(DecodeEveryPrefixOfEveryFrameStaysWellFormed)
{
	for (u32 i = 0; i < ARRAY_COUNT(vectors); i++)
	{
		u8	pdu[MODBUS_MAX_PDU];
		u32	pdu_len = from_hex(vectors[i].pdu, pdu);

		for (u32 f = 0; f < ARRAY_COUNT(framings); f++)
		{
			u8	frame[MODBUS_ASCII_MAX_FRAME];
			u32	len = wrap(framings[f], pdu, pdu_len, frame);

			for (u32 cut = 0; cut <= len; cut++)
			{
				for (u32 mode = 0; mode < 4; mode++)
				{
					decode_framing_t	framing = mode == 0 ? DECODE_FRAMING_AUTO : framings[mode - 1];
					decode_status_t		status;

					for (u32 dir = 0; dir < 3; dir++)
					{
						status = run(frame, cut, framing, (decode_direction_t)dir, false);
						ASSERT_EQ(status == DECODE_OK || status == DECODE_MALFORMED);
						status = run(frame, cut, framing, (decode_direction_t)dir, true);
						ASSERT_EQ(status == DECODE_OK || status == DECODE_MALFORMED);
						if (!json_valid(g_json))
							printf("invalid JSON for %s cut %u: %s\n", vectors[i].pdu, cut, g_json);
						ASSERT_EQ(json_valid(g_json));
					}
				}
			}
		}
	}
}

/* ---- limits ---- */

TEST(DecodeLargestFramesFitTheDocumentedBuffer)
{
	for (u32 code = 0; code < 256; code++)
	{
		for (u32 fill = 0; fill < 3; fill++)
		{
			u8	pdu[MODBUS_MAX_PDU];
			u8	frame[MODBUS_ASCII_MAX_FRAME];

			memset(pdu, fill == 0 ? 0x00 : fill == 1 ? 0xFF : 0x01, sizeof(pdu));
			pdu[0] = (u8)code;
			for (u32 f = 0; f < ARRAY_COUNT(framings); f++)
			{
				u32 len = wrap(framings[f], pdu, sizeof(pdu), frame);

				for (u32 dir = 0; dir < 3; dir++)
				{
					decode_status_t status = run(frame, len, DECODE_FRAMING_AUTO, (decode_direction_t)dir, false);

					ASSERT_EQ(status == DECODE_OK || status == DECODE_MALFORMED);
					ASSERT_EQ(strlen(g_text) < 32768);
					status = run(frame, len, DECODE_FRAMING_AUTO, (decode_direction_t)dir, true);
					ASSERT_EQ(status == DECODE_OK || status == DECODE_MALFORMED);
					ASSERT_EQ(strlen(g_json) < 32768);
					ASSERT_EQ(json_valid(g_json));
				}
			}
		}
	}
}

TEST(DecodeCutsOutputToTheBufferAndNeverOverflows)
{
	u8	pdu[MODBUS_MAX_PDU];
	u8	frame[MODBUS_RTU_MAX_FRAME];
	u32	pdu_len = from_hex("03 06 02 2B 00 00 00 64", pdu);
	u32	len     = wrap(DECODE_FRAMING_RTU, pdu, pdu_len, frame);
	u32	full;

	ASSERT_EQ(run(frame, len, DECODE_FRAMING_RTU, DECODE_DIRECTION_AUTO, false) == DECODE_OK);
	full = (u32)strlen(g_text);
	for (u32 cap = 1; cap <= full + 1; cap++)
	{
		char	buf[512];
		u32	out_len = 0;

		memset(buf, 0xA5, sizeof(buf));
		decode_status_t status = decode_frame(frame, len, DECODE_FRAMING_RTU, DECODE_DIRECTION_AUTO, false, buf,
						      cap, &out_len);

		ASSERT_EQ(status == (cap > full ? DECODE_OK : DECODE_TRUNCATED));
		ASSERT_EQ(out_len < cap);
		ASSERT_EQ(buf[out_len] == '\0');
		ASSERT_EQ(strncmp(buf, g_text, out_len) == 0);
		ASSERT_EQ((u8)buf[cap] == 0xA5);
	}
}

TEST(DecodeRejectsBadArguments)
{
	u8	frame[8] = { 1, 3, 0, 0, 0, 1, 0, 0 };
	char	out[64];
	u32	len = 0;

	ASSERT_EQ(decode_frame(frame, 8, DECODE_FRAMING_RTU, DECODE_DIRECTION_AUTO, false, NULL, 64, &len) == DECODE_INVALID);
	ASSERT_EQ(decode_frame(frame, 8, DECODE_FRAMING_RTU, DECODE_DIRECTION_AUTO, false, out, 0, &len) == DECODE_INVALID);
	ASSERT_EQ(decode_frame(frame, 8, DECODE_FRAMING_RTU, DECODE_DIRECTION_AUTO, false, out, 64, NULL) == DECODE_INVALID);
	ASSERT_EQ(decode_frame(NULL, 8, DECODE_FRAMING_RTU, DECODE_DIRECTION_AUTO, false, out, 64, &len) == DECODE_INVALID);
	ASSERT_EQ(decode_frame(frame, DECODE_MAX_FRAME + 1, DECODE_FRAMING_RTU, DECODE_DIRECTION_AUTO, false, out, 64,
			       &len) == DECODE_INVALID);
}

/* ---- the public function ---- */

TEST(DecodePublicFunctionMapsResults)
{
	static char	out[MOGBUS_DECODE_BUFFER_SIZE];
	u8		pdu[MODBUS_MAX_PDU];
	u8		frame[MODBUS_RTU_MAX_FRAME];
	u32		pdu_len = from_hex("03 00 6B 00 03", pdu);
	u32		len     = wrap(DECODE_FRAMING_RTU, pdu, pdu_len, frame);
	size_t		out_len = 0;

	ASSERT_EQ(mogbus_decode_frame(frame, len, MOGBUS_FRAMING_AUTO, MOGBUS_DIRECTION_AUTO, false, out, sizeof(out),
				      &out_len) == MOGBUS_OK);
	ASSERT_EQ(out_len == strlen(out));
	ASSERT_EQ(strstr(out, "Read Holding Registers") != NULL);
	ASSERT_EQ(mogbus_decode_frame(frame, len, MOGBUS_FRAMING_RTU, MOGBUS_DIRECTION_REQUEST, true, out, sizeof(out),
				      &out_len) == MOGBUS_OK);
	ASSERT_EQ(json_valid(out));
	frame[len - 1] ^= 0xFF;
	ASSERT_EQ(mogbus_decode_frame(frame, len, MOGBUS_FRAMING_AUTO, MOGBUS_DIRECTION_AUTO, false, out, sizeof(out),
				      &out_len) == MOGBUS_ERR_FRAME);
	ASSERT_EQ(strstr(out, "CRC mismatch") != NULL);
	ASSERT_EQ(mogbus_decode_frame(frame, len, MOGBUS_FRAMING_AUTO, MOGBUS_DIRECTION_AUTO, false, out, 16,
				      &out_len) == MOGBUS_ERR_NOMEM);
	ASSERT_EQ(out_len == 15);
	ASSERT_EQ(mogbus_decode_frame(frame, len, MOGBUS_FRAMING_AUTO, MOGBUS_DIRECTION_AUTO, false, NULL, 16,
				      &out_len) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_decode_frame(frame, 5000, MOGBUS_FRAMING_AUTO, MOGBUS_DIRECTION_AUTO, false, out, sizeof(out),
				      &out_len) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_decode_frame(frame, len, (mogbus_framing_t)9, MOGBUS_DIRECTION_AUTO, false, out, sizeof(out),
				      &out_len) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_decode_frame(frame, len, MOGBUS_FRAMING_AUTO, (mogbus_direction_t)9, false, out, sizeof(out),
				      &out_len) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_decode_frame(frame, len, MOGBUS_FRAMING_AUTO, MOGBUS_DIRECTION_AUTO, false, out, sizeof(out),
				      NULL) == MOGBUS_ERR_FRAME);
}
