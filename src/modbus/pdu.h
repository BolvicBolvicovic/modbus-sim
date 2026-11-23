#ifndef PDU_H
#define PDU_H

#include "vlib/c_types.h"
#include "device/device.h"
#include "device/fault.h"
#include "util/rng.h"

/* Supported function codes. */
enum modbus_function_code
{
	FC_READ_COILS                     = 0x01,
	FC_READ_DISCRETE_INPUTS           = 0x02,
	FC_READ_HOLDING_REGISTERS         = 0x03,
	FC_READ_INPUT_REGISTERS           = 0x04,
	FC_WRITE_SINGLE_COIL              = 0x05,
	FC_WRITE_SINGLE_REGISTER          = 0x06,
	FC_READ_EXCEPTION_STATUS          = 0x07,
	FC_DIAGNOSTICS                    = 0x08,
	FC_GET_COM_EVENT_COUNTER          = 0x0B,
	FC_GET_COM_EVENT_LOG              = 0x0C,
	FC_WRITE_MULTIPLE_COILS           = 0x0F,
	FC_WRITE_MULTIPLE_REGISTERS       = 0x10,
	FC_REPORT_SERVER_ID               = 0x11,
	FC_READ_FILE_RECORD               = 0x14,
	FC_WRITE_FILE_RECORD              = 0x15,
	FC_MASK_WRITE_REGISTER            = 0x16,
	FC_READ_WRITE_MULTIPLE_REGISTERS  = 0x17,
	FC_READ_FIFO_QUEUE                = 0x18,
	FC_ENCAPSULATED_INTERFACE_TRANSPORT = 0x2B,
};

/* Public FC 08 diagnostics subfunctions. */
enum modbus_diagnostics_subfunction
{
	DIAG_SUB_RETURN_QUERY_DATA       = 0x0000,
	DIAG_SUB_RESTART_COMMUNICATIONS  = 0x0001,
	DIAG_SUB_READ_DIAGNOSTIC_REGISTER = 0x0002,
	DIAG_SUB_CHANGE_ASCII_DELIMITER  = 0x0003,
	DIAG_SUB_LISTEN_ONLY_MODE        = 0x0004,
	DIAG_SUB_CLEAR_COUNTERS          = 0x000A,
	DIAG_SUB_RETURN_BUS_MESSAGE_COUNT = 0x000B,
	DIAG_SUB_RETURN_BUS_COMM_ERROR_COUNT = 0x000C,
	DIAG_SUB_RETURN_BUS_EXCEPTION_ERROR_COUNT = 0x000D,
	DIAG_SUB_RETURN_SLAVE_MESSAGE_COUNT = 0x000E,
	DIAG_SUB_RETURN_SLAVE_NO_RESPONSE_COUNT = 0x000F,
	DIAG_SUB_RETURN_SLAVE_NAK_COUNT  = 0x0010,
	DIAG_SUB_RETURN_SLAVE_BUSY_COUNT = 0x0011,
	DIAG_SUB_RETURN_BUS_CHAR_OVERRUN_COUNT = 0x0012,
	DIAG_SUB_CLEAR_OVERRUN_COUNTER_AND_FLAG = 0x0014,
};

/* Exception codes. */
enum modbus_exception_code
{
	EXC_ILLEGAL_FUNCTION      = 0x01,
	EXC_ILLEGAL_DATA_ADDRESS  = 0x02,
	EXC_ILLEGAL_DATA_VALUE    = 0x03,
	EXC_DEVICE_BUSY           = 0x06,
	EXC_DEVICE_NAK            = 0x07,
	EXC_GATEWAY_TARGET_FAILED = 0x0B,
};

/* Protocol limits from the Modbus specification. */
#define MB_MAX_READ_BITS		2000
#define MB_MAX_READ_REGISTERS		125
#define MB_MAX_WRITE_BITS		1968
#define MB_MAX_WRITE_REGISTERS		123

/* Name: modbus_handle_pdu
 * Description: runs one request PDU against a device and writes the response PDU.
 * `resp` must hold MODBUS_MAX_PDU bytes. Returns the response length: >= 2 for a
 * normal or exception response, or 0 when the response must be suppressed.
 * Protocol problems become Modbus exception responses; this never fails.
 * */
u32	modbus_handle_pdu(device_t* dev, u64 now_ms, const u8* req, u32 req_len, u8* resp);

/* Handles a PDU and applies device fault decision. */
u32	modbus_handle_request(device_t* dev, rng_t* rng, u64 now_ms,
			      const u8* req, u32 req_len, u8* resp,
			      fault_decision_t* decision);

/* Builds an exception response PDU. Returns its length (2). */
u32	modbus_exception(u8* resp, u8 function, u8 code);

#endif
