/**
 * @file
 * @brief Fuzzes the MBAP path shared by Modbus TCP and UDP: header validation, then the full
 *        request/response with unit routing (unit 1 exists, others get exception 0B).
 *
 * Invariant: a reply is a well-formed MBAP frame whose header length matches its size and
 * whose transaction, protocol and unit echo the request.
 */
#include "fuzz_common.h"
#include "modbus/mbap.h"
#include "net/net_common.h"

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	static u64		now_ms;
	device_t*		devices[] = { fuzz_device() };
	u8			frame[MBAP_MAX_FRAME];
	u8			out[MBAP_MAX_FRAME];
	mbap_header_t		header;
	mbap_header_t		reply;
	fault_decision_t	decision = { 0 };
	u32			len;

	if (size < MBAP_HEADER_SIZE + 1 || size > MBAP_MAX_FRAME)
		return 0;
	memcpy(frame, data, size);
	mbap_decode(frame, &header);
	if (!mbap_is_valid(&header) || mbap_frame_size(&header) != size)
		return 0;
	now_ms += 3;
	len = net_mbap_respond(NULL, devices, 1, fuzz_rng(), now_ms, "fuzz", &header,
			       frame + MBAP_HEADER_SIZE, out, &decision);
	if (len == 0)
		return 0;
	FUZZ_CHECK(len > MBAP_HEADER_SIZE && len <= MBAP_MAX_FRAME);
	mbap_decode(out, &reply);
	FUZZ_CHECK(mbap_frame_size(&reply) == len);
	FUZZ_CHECK(reply.transaction == header.transaction && reply.protocol == 0 && reply.unit == header.unit);
	return 0;
}
