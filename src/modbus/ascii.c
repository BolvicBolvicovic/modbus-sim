/**
 * @file
 * @brief Modbus ASCII framing: hex encoding, LRC and delimiter scanning.
 */
#include "modbus/ascii.h"

static const char hex_digits[] = "0123456789ABCDEF";

static bool
hex_value(u8 c, u8* value)
{
	if (c >= '0' && c <= '9')
	{
		*value = (u8)(c - '0');
		return true;
	}
	if (c >= 'a' && c <= 'f')
	{
		*value = (u8)(c - 'a' + 10);
		return true;
	}
	if (c >= 'A' && c <= 'F')
	{
		*value = (u8)(c - 'A' + 10);
		return true;
	}
	return false;
}

u8
modbus_ascii_lrc(const u8* buf, u32 len)
{
	u8 sum = 0;

	for (u32 i = 0; i < len; i++)
		sum = (u8)(sum + buf[i]);
	return (u8)(0u - sum);
}

u32
modbus_ascii_encode(u8* buf, u8 addr, const u8* pdu, u32 pdu_len)
{
	u8	binary[1 + MODBUS_MAX_PDU];
	u8	lrc;
	u32	pos = 0;

	if (pdu_len < 1 || pdu_len > MODBUS_MAX_PDU)
		return 0;

	binary[0] = addr;
	for (u32 i = 0; i < pdu_len; i++)
		binary[1 + i] = pdu[i];
	lrc = modbus_ascii_lrc(binary, 1 + pdu_len);

	buf[pos++] = ':';
	buf[pos++] = (u8)hex_digits[addr >> 4];
	buf[pos++] = (u8)hex_digits[addr & 0x0F];
	for (u32 i = 0; i < pdu_len; i++)
	{
		buf[pos++] = (u8)hex_digits[pdu[i] >> 4];
		buf[pos++] = (u8)hex_digits[pdu[i] & 0x0F];
	}
	buf[pos++] = (u8)hex_digits[lrc >> 4];
	buf[pos++] = (u8)hex_digits[lrc & 0x0F];
	buf[pos++] = '\r';
	buf[pos++] = '\n';
	return pos;
}

void
modbus_ascii_corrupt_lrc(u8* frame, u32 len)
{
	u8* digit;

	if (len < 5)
		return;
	digit = frame + len - 3;
	if (*digit == '9')
		*digit = 'A';
	else if (*digit == 'F')
		*digit = '0';
	else if ((*digit >= '0' && *digit <= '8') ||
		 (*digit >= 'A' && *digit <= 'E'))
		*digit = (u8)(*digit + 1);
}

bool
modbus_ascii_decode(const u8* frame, u32 len, u8* out, u8* addr,
		    const u8** pdu, u32* pdu_len)
{
	u32	hex_len;
	u32	out_len;
	u32	pos = 1;

	if (len < 9 || len > MODBUS_ASCII_MAX_FRAME)
		return false;
	if (frame[0] != ':' || frame[len - 2] != '\r' || frame[len - 1] != '\n')
		return false;

	hex_len = len - 3; /* colon and CRLF */
	if (hex_len % 2 != 0)
		return false;
	out_len = hex_len / 2;
	if (out_len < 3 || out_len > MODBUS_MAX_PDU + 2)
		return false;

	for (u32 i = 0; i < out_len; i++)
	{
		u8	hi = 0;
		u8	lo = 0;

		if (!hex_value(frame[pos], &hi) || !hex_value(frame[pos + 1], &lo))
			return false;
		out[i] = (u8)((hi << 4) | lo);
		pos += 2;
	}

	/* out = [addr][PDU...][LRC]; the LRC covers everything before itself. */
	if (modbus_ascii_lrc(out, out_len - 1) != out[out_len - 1])
		return false;

	*addr = out[0];
	*pdu = out + 1;
	*pdu_len = out_len - 2;
	return true;
}

modbus_ascii_scan_t
modbus_ascii_scan(const u8* rx, u32 rx_len, u32* len)
{
	if (rx_len == 0)
		return MODBUS_ASCII_SCAN_NONE;

	if (rx[0] != ':')
	{
		u32 prefix_len = rx_len;

		for (u32 i = 1; i < rx_len; i++)
		{
			if (rx[i] == ':')
			{
				prefix_len = i;
				break;
			}
		}
		for (u32 i = 0; i < prefix_len; i++)
		{
			if (rx[i] != '\r' &&
			    rx[i] != '\n' &&
			    rx[i] != ' ')
			{
				*len = prefix_len;
				return MODBUS_ASCII_SCAN_DISCARD;
			}
		}
		*len = prefix_len;
		return MODBUS_ASCII_SCAN_SKIP;
	}

	for (u32 i = 1; i < rx_len; i++)
	{
		if (rx[i] == ':')
		{
			/* A second start byte means the current prefix is a truncated
			 * frame; drop it and rescan from the new colon. */
			*len = i;
			return MODBUS_ASCII_SCAN_DISCARD;
		}
		if (rx[i] != '\r' && rx[i] != '\n')
			continue;

		/* A lone LF, or a CR followed by a non-LF byte, is malformed; a CR at
		 * the end of the buffer may still be completed by the next read. */
		if (rx[i] == '\n' ||
		    (i + 1 < rx_len && rx[i + 1] != '\n'))
		{
			*len = i + 1;
			return MODBUS_ASCII_SCAN_DISCARD;
		}
		if (i + 1 >= rx_len)
			return MODBUS_ASCII_SCAN_NONE;

		*len = i + 2;
		if (*len > MODBUS_ASCII_MAX_FRAME)
			return MODBUS_ASCII_SCAN_DISCARD;
		return MODBUS_ASCII_SCAN_FRAME;
	}

	if (rx_len < MODBUS_ASCII_MAX_FRAME)
		return MODBUS_ASCII_SCAN_NONE;

	*len = rx_len;
	return MODBUS_ASCII_SCAN_DISCARD;
}
