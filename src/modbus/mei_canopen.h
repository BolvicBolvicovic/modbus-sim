/**
 * @file
 * @brief CANopen General Reference (FC 43 MEI 0x0D) request parsing and responses.
 */
#ifndef MODBUS_MEI_CANOPEN_H
#define MODBUS_MEI_CANOPEN_H

#include "vlib/c_types.h"
#include <stdbool.h>

typedef struct canopen_request_t canopen_request_t;

#define MEI_TYPE_CANOPEN	0x0D

/** @brief CiA 309-2 numbers protocol control bits MSB-first; these are the byte masks. */
enum canopen_protocol_control
{
	CANOPEN_CTRL_ACCESS        = 0x01, /**< 0 = upload (read), 1 = download (write) */
	CANOPEN_CTRL_ENCODED_DATA  = 0x02,
	CANOPEN_CTRL_NETWORK_ID    = 0x04,
	CANOPEN_CTRL_COUNTER       = 0x20,
	CANOPEN_CTRL_EXTENDED_CTRL = 0x40,
	CANOPEN_CTRL_EXTEND        = 0x80,
};

#define CANOPEN_CTRL_UNSUPPORTED \
	(CANOPEN_CTRL_ENCODED_DATA | CANOPEN_CTRL_COUNTER | CANOPEN_CTRL_EXTENDED_CTRL | CANOPEN_CTRL_EXTEND)

/** @brief CANopen service error codes carried in the 4-byte extended exception field. */
enum canopen_error_code
{
	CANOPEN_ERR_OBJECT_NOT_FOUND     = 0xFFFF000Bu,
	CANOPEN_ERR_INVALID_COUNT        = 0xFFFF0011u,
	CANOPEN_ERR_LENGTH_TOO_HIGH      = 0x06070012u,
	CANOPEN_ERR_ACCESS_REFUSED       = 0x08000020u,	/**< the access hook vetoed: data cannot be transferred or stored to the application */
	CANOPEN_ERR_RESPONSE_TOO_LARGE   = 0xFFFF001Eu,
	CANOPEN_ERR_SERVICE_NOT_SUPPORTED = 0xFFFF1003u,
};

/** @brief A decoded CANopen General Reference request. */
struct canopen_request_t
{
	u8	control;
	u8	reserved;
	u8	network_id;
	u8	data_type;
	u8	node;
	u16	index;
	u8	subindex;
	u16	start;
	u16	count;
	u32	data_offset;
	u32	data_len;
	bool	write;
	bool	has_network_id;
	bool	has_data_type;
};

/**
 * @brief Computes the full PDU length from a (possibly partial) request buffer.
 *
 * Only the header bytes are read, so RTU framing can split variable-length frames.
 */
bool	canopen_expected_pdu_len(const u8* pdu, u32 pdu_len, u32* expected);

/** @brief Parses the header without checking the trailing data length. */
bool	canopen_parse_header(const u8* pdu, u32 pdu_len, canopen_request_t* out);

/** @brief Parses a complete request, rejecting any mismatch between count and data. */
bool	canopen_parse_request(const u8* pdu, u32 pdu_len, canopen_request_t* out);

/**
 * @name Extended exception responses
 * Both write `AB FF <len-hi> <len-lo> 0D <AE|CE> <payload>` into `resp` and return its length.
 * @{
 */
u32	canopen_unsupported_option_exception(u8* resp, u8 supported_control);
u32	canopen_service_exception(u8* resp, u32 error_code);
/** @} */

#endif
