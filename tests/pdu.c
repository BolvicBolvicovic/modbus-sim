#include "vtest.h"
#include "modbus/pdu.h"
#include "modbus/mbap.h"
#include <string.h>

/* Builds a device with: holding 0 = 0x1234, holding 1 = 0x5678, input 0 = 7,
 * coils 0..9 off, discrete 0..2 = on,off,on. */
static device_t*
make_device(arena_t* arena)
{
	device_t*	dev = device_new(arena, 0);
	point_t		p   = { 0 };

	p.kind = POINT_STATIC;
	p.space = SPACE_HOLDING; p.type = VT_U16; p.address = 0; p.static_value = 0x1234;
	device_add_point(dev, &p);
	p.address = 1; p.static_value = 0x5678;
	device_add_point(dev, &p);
	p.space = SPACE_INPUT; p.address = 0; p.static_value = 7;
	device_add_point(dev, &p);

	p.type = VT_BOOL;
	p.space = SPACE_COIL; p.static_value = 0;
	for (u32 i = 0; i < 10; i++) { p.address = i; device_add_point(dev, &p); }
	p.space = SPACE_DISCRETE;
	p.address = 0; p.static_value = 1; device_add_point(dev, &p);
	p.address = 1; p.static_value = 0; device_add_point(dev, &p);
	p.address = 2; p.static_value = 1; device_add_point(dev, &p);
	return dev;
}

static u32
run(device_t* dev, const u8* req, u32 len, u8* resp)
{
	return modbus_handle_pdu(dev, 0, req, len, resp);
}

TEST(ReadHoldingRegisters)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { 0x03, 0x00, 0x00, 0x00, 0x02 };
	u8		resp[MODBUS_MAX_PDU];
	u8		want[] = { 0x03, 0x04, 0x12, 0x34, 0x56, 0x78 };

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(want));
	ASSERT_EQ(memcmp(resp, want, sizeof(want)) == 0);
	arena_release(arena);
}

TEST(ReadInputRegister)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { 0x04, 0x00, 0x00, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 4);
	ASSERT_EQ(resp[2] == 0x00 && resp[3] == 0x07);
	arena_release(arena);
}

TEST(ReadDiscreteInputsPacksBitsLsbFirst)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { 0x02, 0x00, 0x00, 0x00, 0x03 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 3);
	ASSERT_EQ(resp[1] == 1 && resp[2] == 0x05);	/* on,off,on = 0b101 */
	arena_release(arena);
}

TEST(WriteSingleCoilThenRead)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		write[] = { 0x05, 0x00, 0x03, 0xFF, 0x00 };
	u8		read[]  = { 0x01, 0x00, 0x00, 0x00, 0x0A };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, write, sizeof(write), resp) == 5);
	ASSERT_EQ(memcmp(resp, write, 5) == 0);
	ASSERT_EQ(run(dev, read, sizeof(read), resp) == 4);
	ASSERT_EQ(resp[1] == 2 && resp[2] == 0x08 && resp[3] == 0x00);
	arena_release(arena);
}

TEST(WriteSingleCoilRejectsOddValue)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { 0x05, 0x00, 0x00, 0x12, 0x34 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 2);
	ASSERT_EQ(resp[0] == 0x85 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	arena_release(arena);
}

TEST(WriteMultipleRegisters)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		write[] = { 0x10, 0x00, 0x00, 0x00, 0x02, 0x04, 0xAA, 0xBB, 0xCC, 0xDD };
	u8		read[]  = { 0x03, 0x00, 0x00, 0x00, 0x02 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, write, sizeof(write), resp) == 5);
	ASSERT_EQ(resp[0] == 0x10 && resp[4] == 0x02);
	run(dev, read, sizeof(read), resp);
	ASSERT_EQ(resp[2] == 0xAA && resp[3] == 0xBB && resp[4] == 0xCC && resp[5] == 0xDD);
	arena_release(arena);
}

TEST(WriteMultipleCoils)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		write[] = { 0x0F, 0x00, 0x00, 0x00, 0x0A, 0x02, 0xFF, 0x01 };
	u8		read[]  = { 0x01, 0x00, 0x00, 0x00, 0x0A };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, write, sizeof(write), resp) == 5);
	run(dev, read, sizeof(read), resp);
	ASSERT_EQ(resp[2] == 0xFF && resp[3] == 0x01);
	arena_release(arena);
}

TEST(WriteToReadOnlyTableIsAnAddressError)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { 0x06, 0x00, 0x05, 0x00, 0x01 };	/* holding 5 is not defined */
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 2);
	ASSERT_EQ(resp[0] == 0x86 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	arena_release(arena);
}

TEST(ReadUndefinedAddressIsAnAddressError)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { 0x03, 0x00, 0x01, 0x00, 0x02 };	/* 1 ok, 2 undefined */
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 2);
	ASSERT_EQ(resp[0] == 0x83 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	arena_release(arena);
}

TEST(QuantityLimitsAreValueErrors)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		zero[]  = { 0x03, 0x00, 0x00, 0x00, 0x00 };
	u8		big[]   = { 0x03, 0x00, 0x00, 0x00, 0x7E };	/* 126 > 125 */
	u8		resp[MODBUS_MAX_PDU];

	run(dev, zero, sizeof(zero), resp);
	ASSERT_EQ(resp[0] == 0x83 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, big, sizeof(big), resp);
	ASSERT_EQ(resp[0] == 0x83 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	arena_release(arena);
}

TEST(ShortOrMismatchedRequestsAreValueErrors)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		short_req[]   = { 0x03, 0x00 };
	u8		bad_count[]   = { 0x10, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x01 };	/* bytes != 2*qty */
	u8		resp[MODBUS_MAX_PDU];

	run(dev, short_req, sizeof(short_req), resp);
	ASSERT_EQ(resp[0] == 0x83 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_count, sizeof(bad_count), resp);
	ASSERT_EQ(resp[0] == 0x90 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	arena_release(arena);
}

TEST(UnknownFunctionIsIllegalFunction)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { 0x2B, 0x0E, 0x01, 0x00 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 2);
	ASSERT_EQ(resp[0] == 0xAB && resp[1] == EXC_ILLEGAL_FUNCTION);
	arena_release(arena);
}

TEST(ReadExceptionStatusReturnsConfiguredByte)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { FC_READ_EXCEPTION_STATUS };
	u8		resp[MODBUS_MAX_PDU];

	dev->exception_status = 0x5A;
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 2);
	ASSERT_EQ(resp[0] == FC_READ_EXCEPTION_STATUS && resp[1] == 0x5A);
	arena_release(arena);
}

TEST(ReadExceptionStatusDefaultsToZero)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { 0x07 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 2);
	ASSERT_EQ(resp[0] == 0x07 && resp[1] == 0x00);
	arena_release(arena);
}

TEST(ReadExceptionStatusRejectsMalformedRequest)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		extra[] = { 0x07, 0x00 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, extra, sizeof(extra), resp) == 2);
	ASSERT_EQ(resp[0] == 0x87 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	arena_release(arena);
}

TEST(ReportServerIdReturnsConfiguredValues)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { FC_REPORT_SERVER_ID };
	u8		resp[MODBUS_MAX_PDU];

	dev->server_id     = 0x2A;
	dev->run_indicator = 0x00;
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 4);
	ASSERT_EQ(resp[0] == FC_REPORT_SERVER_ID);
	ASSERT_EQ(resp[1] == 2);
	ASSERT_EQ(resp[2] == 0x2A && resp[3] == 0x00);
	arena_release(arena);
}

TEST(ReportServerIdDefaults)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { 0x11 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 4);
	ASSERT_EQ(resp[0] == 0x11 && resp[1] == 2);
	ASSERT_EQ(resp[2] == 0x00 && resp[3] == 0xFF);
	arena_release(arena);
}

TEST(ReportServerIdRejectsMalformedRequest)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		extra[] = { FC_REPORT_SERVER_ID, 0x00 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, extra, sizeof(extra), resp) == 2);
	ASSERT_EQ(resp[0] == (u8)(FC_REPORT_SERVER_ID | 0x80));
	ASSERT_EQ(resp[1] == EXC_ILLEGAL_DATA_VALUE);
	arena_release(arena);
}

TEST(GetComEventCounterReturnsStatusAndCount)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		read[] = { 0x03, 0x00, 0x00, 0x00, 0x01 };
	u8		req[]  = { FC_GET_COM_EVENT_COUNTER };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, read, sizeof(read), resp) == 4);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(resp[0] == FC_GET_COM_EVENT_COUNTER);
	ASSERT_EQ(resp[1] == 0x00 && resp[2] == 0x00);
	ASSERT_EQ(resp[3] == 0x00 && resp[4] == 0x02);
	arena_release(arena);
}

TEST(GetComEventCounterCountsHandledRequests)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { FC_GET_COM_EVENT_COUNTER };
	u8		bad[]  = { 0x03, 0x00, 0x00, 0x00, 0x00 };	/* illegal quantity -> exception */
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(resp[3] == 0x00 && resp[4] == 0x01);
	run(dev, bad, sizeof(bad), resp);
	ASSERT_EQ(resp[0] == 0x83 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(resp[3] == 0x00 && resp[4] == 0x03);
	arena_release(arena);
}

TEST(GetComEventCounterWrapsAt16Bits)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { FC_GET_COM_EVENT_COUNTER };
	u8		resp[MODBUS_MAX_PDU];

	dev->com_event_count = 0xFFFF;
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(resp[1] == 0x00 && resp[2] == 0x00);
	ASSERT_EQ(resp[3] == 0x00 && resp[4] == 0x00);
	arena_release(arena);
}

TEST(GetComEventCounterRejectsMalformedRequest)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		extra[] = { FC_GET_COM_EVENT_COUNTER, 0x00 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, extra, sizeof(extra), resp) == 2);
	ASSERT_EQ(resp[0] == (u8)(FC_GET_COM_EVENT_COUNTER | 0x80));
	ASSERT_EQ(resp[1] == EXC_ILLEGAL_DATA_VALUE);
	arena_release(arena);
}

TEST(GetComEventLogReturnsCountsAndEmptyLog)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		read[] = { 0x03, 0x00, 0x00, 0x00, 0x01 };
	u8		req[]  = { FC_GET_COM_EVENT_LOG };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, read, sizeof(read), resp) == 4);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 8);
	ASSERT_EQ(resp[0] == FC_GET_COM_EVENT_LOG);
	ASSERT_EQ(resp[1] == 6);
	ASSERT_EQ(resp[2] == 0x00 && resp[3] == 0x00);
	ASSERT_EQ(resp[4] == 0x00 && resp[5] == 0x02);
	ASSERT_EQ(resp[6] == 0x00 && resp[7] == 0x02);
	arena_release(arena);
}

TEST(GetComEventLogMatchesComEventCounter)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		fc11[] = { FC_GET_COM_EVENT_COUNTER };
	u8		fc12[] = { FC_GET_COM_EVENT_LOG };
	u8		bad[]  = { 0x03, 0x00, 0x00, 0x00, 0x00 };	/* illegal quantity -> exception */
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, fc11, sizeof(fc11), resp) == 5);
	ASSERT_EQ(resp[3] == 0x00 && resp[4] == 0x01);
	run(dev, bad, sizeof(bad), resp);
	ASSERT_EQ(resp[0] == 0x83 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	ASSERT_EQ(run(dev, fc12, sizeof(fc12), resp) == 8);
	ASSERT_EQ(resp[1] == 6);
	ASSERT_EQ(resp[4] == 0x00 && resp[5] == 0x03);
	ASSERT_EQ(resp[6] == 0x00 && resp[7] == 0x03);
	ASSERT_EQ(run(dev, fc11, sizeof(fc11), resp) == 5);
	ASSERT_EQ(resp[3] == 0x00 && resp[4] == 0x04);
	arena_release(arena);
}

TEST(GetComEventLogWrapsAt16Bits)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { FC_GET_COM_EVENT_LOG };
	u8		resp[MODBUS_MAX_PDU];

	dev->com_event_count = 0xFFFF;
	dev->message_count   = 0xFFFF;
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 8);
	ASSERT_EQ(resp[1] == 6);
	ASSERT_EQ(resp[2] == 0x00 && resp[3] == 0x00);
	ASSERT_EQ(resp[4] == 0x00 && resp[5] == 0x00);
	ASSERT_EQ(resp[6] == 0x00 && resp[7] == 0x00);
	arena_release(arena);
}

TEST(GetComEventLogRejectsMalformedRequest)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		extra[] = { FC_GET_COM_EVENT_LOG, 0x00 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, extra, sizeof(extra), resp) == 2);
	ASSERT_EQ(resp[0] == (u8)(FC_GET_COM_EVENT_LOG | 0x80));
	ASSERT_EQ(resp[1] == EXC_ILLEGAL_DATA_VALUE);
	arena_release(arena);
}

TEST(DiagnosticsReturnQueryDataEchoesRequest)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { FC_DIAGNOSTICS, 0x00, 0x00, 0x12, 0x34 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(memcmp(resp, req, 5) == 0);
	arena_release(arena);
}

TEST(DiagnosticsRejectsMalformedRequest)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		short_req[] = { FC_DIAGNOSTICS, 0x00, 0x00, 0x12 };
	u8		extra[]     = { FC_DIAGNOSTICS, 0x00, 0x00, 0x12, 0x34, 0x00 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, short_req, sizeof(short_req), resp) == 2);
	ASSERT_EQ(resp[0] == 0x88 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	ASSERT_EQ(run(dev, extra, sizeof(extra), resp) == 2);
	ASSERT_EQ(resp[0] == 0x88 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	arena_release(arena);
}

TEST(DiagnosticsUnsupportedSubfunctionIsIllegalFunction)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { FC_DIAGNOSTICS, 0x00, 0x01, 0x00, 0x00 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 2);
	ASSERT_EQ(resp[0] == 0x88 && resp[1] == EXC_ILLEGAL_FUNCTION);
	arena_release(arena);
}

TEST(MbapHeaderRoundTripAndValidation)
{
	u8		buf[MBAP_HEADER_SIZE];
	mbap_header_t	in  = { .transaction = 0xABCD, .protocol = 0, .length = 6, .unit = 17 };
	mbap_header_t	out = { 0 };

	mbap_encode(buf, &in);
	mbap_decode(buf, &out);
	ASSERT_EQ(out.transaction == 0xABCD && out.length == 6 && out.unit == 17);
	ASSERT_SUCCESS(mbap_is_valid(&out));
	ASSERT_EQ(mbap_frame_size(&out) == 12);

	out.protocol = 1;
	ASSERT_FAILURE(mbap_is_valid(&out));
	out.protocol = 0; out.length = 1;
	ASSERT_FAILURE(mbap_is_valid(&out));
	out.length = 255;
	ASSERT_FAILURE(mbap_is_valid(&out));
}

static device_t*
make_file_device(arena_t* arena)
{
	device_t*	dev    = device_new(arena, 0);
	u16		data1[] = { 0x1234, 0x5678 };
	u16		data2[] = { 0x000A, 0x000B, 0x000C };
	u16		data3[] = { 0x00FF };

	device_add_file_record(dev, 1, 0, 2, data1);
	device_add_file_record(dev, 1, 10, 3, data2);
	device_add_file_record(dev, 4, 2, 1, data3);
	return dev;
}

TEST(ReadFileRecordSingleSubRequest)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_file_device(arena);
	u8		req[]  = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02 };
	u8		resp[MODBUS_MAX_PDU];
	u8		want[] = { FC_READ_FILE_RECORD, 0x06, 0x05, 0x06, 0x12, 0x34, 0x56, 0x78 };

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(want));
	ASSERT_EQ(memcmp(resp, want, sizeof(want)) == 0);
	arena_release(arena);
}

TEST(ReadFileRecordMultipleSubRequests)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_file_device(arena);
	u8		req[]  = {
		FC_READ_FILE_RECORD, 0x0E,
		0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,	/* file 1, rec 0, len 1 */
		0x06, 0x00, 0x04, 0x00, 0x02, 0x00, 0x01,	/* file 4, rec 2, len 1 */
	};
	u8		resp[MODBUS_MAX_PDU];
	u8		want[] = {
		FC_READ_FILE_RECORD, 0x08,
		0x03, 0x06, 0x12, 0x34,
		0x03, 0x06, 0x00, 0xFF,
	};

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(want));
	ASSERT_EQ(memcmp(resp, want, sizeof(want)) == 0);
	arena_release(arena);
}

TEST(ReadFileRecordRejectsMalformedRequest)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_file_device(arena);
	u8		short_req[] = { FC_READ_FILE_RECORD };
	u8		bad_count[] = { FC_READ_FILE_RECORD, 0x06, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01 };
	u8		bad_mod7[]  = { FC_READ_FILE_RECORD, 0x08, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00 };
	u8		bad_high[]  = { FC_READ_FILE_RECORD, 0xF6, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01 };
	u8		bad_len[]   = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00 };
	u8		bad_ref[]   = { FC_READ_FILE_RECORD, 0x07, 0x05, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01 };
	u8		bad_rlen[]  = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 };
	u8		bad_rec[]   = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x27, 0x10, 0x00, 0x01 };
	u8		big_resp[]  = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x7E };
	u8		resp[MODBUS_MAX_PDU];

	run(dev, short_req, sizeof(short_req), resp);
	ASSERT_EQ(resp[0] == 0x94 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_count, sizeof(bad_count), resp);
	ASSERT_EQ(resp[0] == 0x94 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_mod7, sizeof(bad_mod7), resp);
	ASSERT_EQ(resp[0] == 0x94 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_high, sizeof(bad_high), resp);
	ASSERT_EQ(resp[0] == 0x94 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_len, sizeof(bad_len), resp);
	ASSERT_EQ(resp[0] == 0x94 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_ref, sizeof(bad_ref), resp);
	ASSERT_EQ(resp[0] == 0x94 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_rlen, sizeof(bad_rlen), resp);
	ASSERT_EQ(resp[0] == 0x94 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_rec, sizeof(bad_rec), resp);
	ASSERT_EQ(resp[0] == 0x94 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, big_resp, sizeof(big_resp), resp);
	ASSERT_EQ(resp[0] == 0x94 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	arena_release(arena);
}

TEST(ReadFileRecordUnavailableIsAddressError)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_file_device(arena);
	u8		missing_file[] = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01 };
	u8		missing_rec[]  = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01 };
	u8		gap[]          = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03 };
	u8		resp[MODBUS_MAX_PDU];

	run(dev, missing_file, sizeof(missing_file), resp);
	ASSERT_EQ(resp[0] == 0x94 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	run(dev, missing_rec, sizeof(missing_rec), resp);
	ASSERT_EQ(resp[0] == 0x94 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	/* Range 0-2 spans undefined record 2 between configured segments. */
	run(dev, gap, sizeof(gap), resp);
	ASSERT_EQ(resp[0] == 0x94 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	arena_release(arena);
}

TEST(ReadFileRecordUnavailableWhenNoFileRecordsConfigured)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];

	run(dev, req, sizeof(req), resp);
	ASSERT_EQ(resp[0] == 0x94 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	arena_release(arena);
}
