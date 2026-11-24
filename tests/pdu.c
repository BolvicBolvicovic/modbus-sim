#include "vtest.h"
#include "modbus/pdu.h"
#include "modbus/mbap.h"
#include "modbus/bytes.h"
#include "modbus/mei_canopen.h"
#include "vlib/macros.h"
#include <string.h>

static void
set_fault(device_t* dev, fault_kind_t kind, u32 delay_ms, u8 code, u32 percent)
{
	dev->faults.by_kind[kind] = (fault_t){
		.delay_ms       = delay_ms,
		.exception_code = code,
		.percent        = percent,
	};
	dev->faults.present |= FAULT_BIT(kind);
}

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

static u32
run_request(device_t* dev, rng_t* rng, const u8* req, u32 len, u8* resp,
	    fault_decision_t* decision)
{
	return modbus_handle_request(dev, rng, 0, req, len, resp, decision);
}

static bool
exception_response_is(const u8* resp, u32 len, u8 code)
{
	return len == 2 && resp[0] == (u8)(FC_DIAGNOSTICS | 0x80) && resp[1] == code;
}

static u32
diagnostic_request(u8* req, u16 subfunction, u16 data)
{
	req[0] = FC_DIAGNOSTICS;
	put_be16(req + 1, subfunction);
	put_be16(req + 3, data);
	return 5;
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
	u8		req[]  = { 0x44, 0x00, 0x01, 0x00 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 2);
	ASSERT_EQ(resp[0] == 0xC4 && resp[1] == EXC_ILLEGAL_FUNCTION);
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

TEST(ReportServerIdReturnsConfiguredAdditionalData)
{
	arena_t*	arena  = ARENA_ALLOC();
	device_t*	dev    = make_device(arena);
	u8		req[]  = { FC_REPORT_SERVER_ID };
	u8		resp[MODBUS_MAX_PDU];
	u8		extra[] = { 0xDE, 0xAD, 0xBE, 0xEF };

	dev->server_id     = 0x2A;
	dev->run_indicator = 0x00;
	dev->server_id_additional_data_len = sizeof(extra);
	memcpy(dev->server_id_additional_data, extra, sizeof(extra));

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 4 + sizeof(extra));
	ASSERT_EQ(resp[0] == FC_REPORT_SERVER_ID && resp[1] == 2 + sizeof(extra));
	ASSERT_EQ(resp[2] == 0x2A && resp[3] == 0x00);
	ASSERT_EQ(memcmp(resp + 4, extra, sizeof(extra)) == 0);
	arena_release(arena);
}

TEST(ReportServerIdAcceptsMaxAdditionalData)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[] = { FC_REPORT_SERVER_ID };
	u8		resp[MODBUS_MAX_PDU];

	dev->server_id_additional_data_len = DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX;
	for (u32 i = 0; i < DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX; i++)
		dev->server_id_additional_data[i] = (u8)(0xA0 + i % 16);

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == MODBUS_MAX_PDU);
	ASSERT_EQ(resp[0] == FC_REPORT_SERVER_ID);
	ASSERT_EQ(resp[1] == 2 + DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX);
	ASSERT_EQ(resp[2] == dev->server_id && resp[3] == dev->run_indicator);
	ASSERT_EQ(memcmp(resp + 4, dev->server_id_additional_data,
			 DEVICE_SERVER_ID_ADDITIONAL_DATA_MAX) == 0);
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
	ASSERT_EQ(resp[3] == 0x00 && resp[4] == 0x01);
	arena_release(arena);
}

TEST(GetComEventCounterCountsSuccessfulCompletions)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		read[] = { 0x03, 0x00, 0x00, 0x00, 0x01 };
	u8		req[]  = { FC_GET_COM_EVENT_COUNTER };
	u8		bad[]  = { 0x03, 0x00, 0x00, 0x00, 0x00 };	/* illegal quantity -> exception */
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(resp[3] == 0x00 && resp[4] == 0x00);
	ASSERT_EQ(run(dev, read, sizeof(read), resp) == 4);
	run(dev, bad, sizeof(bad), resp);
	ASSERT_EQ(resp[0] == 0x83 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(resp[3] == 0x00 && resp[4] == 0x01);
	arena_release(arena);
}

TEST(GetComEventCounterWrapsAt16Bits)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		read[] = { 0x03, 0x00, 0x00, 0x00, 0x01 };
	u8		req[]  = { FC_GET_COM_EVENT_COUNTER };
	u8		resp[MODBUS_MAX_PDU];

	dev->com_event_count = 0xFFFF;
	ASSERT_EQ(run(dev, read, sizeof(read), resp) == 4);
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
	ASSERT_EQ(resp[4] == 0x00 && resp[5] == 0x01);
	ASSERT_EQ(resp[6] == 0x00 && resp[7] == 0x02);
	arena_release(arena);
}

TEST(GetComEventLogMatchesComEventCounter)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		read[] = { 0x03, 0x00, 0x00, 0x00, 0x01 };
	u8		fc11[] = { FC_GET_COM_EVENT_COUNTER };
	u8		fc12[] = { FC_GET_COM_EVENT_LOG };
	u8		bad[]  = { 0x03, 0x00, 0x00, 0x00, 0x00 };	/* illegal quantity -> exception */
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, read, sizeof(read), resp) == 4);
	ASSERT_EQ(run(dev, fc11, sizeof(fc11), resp) == 5);
	ASSERT_EQ(resp[3] == 0x00 && resp[4] == 0x01);
	run(dev, bad, sizeof(bad), resp);
	ASSERT_EQ(resp[0] == 0x83 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	ASSERT_EQ(run(dev, fc12, sizeof(fc12), resp) == 8);
	ASSERT_EQ(resp[1] == 6);
	ASSERT_EQ(resp[4] == 0x00 && resp[5] == 0x01);
	ASSERT_EQ(resp[6] == 0x00 && resp[7] == 0x04);
	ASSERT_EQ(run(dev, fc11, sizeof(fc11), resp) == 5);
	ASSERT_EQ(resp[3] == 0x00 && resp[4] == 0x01);
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
	ASSERT_EQ(resp[4] == 0xFF && resp[5] == 0xFF);
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

TEST(GetComEventCounterReportsConfiguredStatus)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[] = { FC_GET_COM_EVENT_COUNTER };
	u8		resp[MODBUS_MAX_PDU];

	dev->com_status = 0xFFFF;
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(resp[0] == FC_GET_COM_EVENT_COUNTER);
	ASSERT_EQ(resp[1] == 0xFF && resp[2] == 0xFF);
	ASSERT_EQ(resp[3] == 0x00 && resp[4] == 0x00); /* FC 11 is not an event */
	arena_release(arena);
}

TEST(GetComEventLogReportsConfiguredStatusAndEvents)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[] = { FC_GET_COM_EVENT_LOG };
	u8		resp[MODBUS_MAX_PDU];

	dev->com_status         = 0xFFFF;
	dev->com_events[0]      = 0x01;
	dev->com_events[1]      = 0x80;
	dev->com_events[2]      = 0xFF;
	dev->com_event_log_count = 3;

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 11);
	ASSERT_EQ(resp[0] == FC_GET_COM_EVENT_LOG);
	ASSERT_EQ(resp[1] == 9);
	ASSERT_EQ(resp[2] == 0xFF && resp[3] == 0xFF);
	ASSERT_EQ(resp[4] == 0x00 && resp[5] == 0x00); /* no events counted */
	ASSERT_EQ(resp[6] == 0x00 && resp[7] == 0x01); /* message count */
	ASSERT_EQ(resp[8] == 0x01 && resp[9] == 0x80 && resp[10] == 0xFF);
	arena_release(arena);
}

TEST(GetComEventLogReportsMaximumEvents)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[] = { FC_GET_COM_EVENT_LOG };
	u8		resp[MODBUS_MAX_PDU];

	for (u32 i = 0; i < DEVICE_MAX_COM_EVENTS; i++)
	{
		dev->com_events[i] = (u8)(i + 1);
		dev->com_event_log_count++;
	}

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 8 + DEVICE_MAX_COM_EVENTS);
	ASSERT_EQ(resp[0] == FC_GET_COM_EVENT_LOG);
	ASSERT_EQ(resp[1] == 6 + DEVICE_MAX_COM_EVENTS);
	ASSERT_EQ(resp[2] == 0x00 && resp[3] == 0x00);
	for (u32 i = 0; i < DEVICE_MAX_COM_EVENTS; i++)
		ASSERT_EQ(resp[8 + i] == (u8)(i + 1));
	arena_release(arena);
}

TEST(GetComEventFunctionsRejectMalformedWithConfiguredValues)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		fc11_extra[] = { FC_GET_COM_EVENT_COUNTER, 0x00 };
	u8		fc12_extra[] = { FC_GET_COM_EVENT_LOG, 0x00 };
	u8		req[]        = { FC_GET_COM_EVENT_LOG };
	u8		resp[MODBUS_MAX_PDU];

	dev->com_status          = 0xFFFF;
	dev->com_events[0]       = 0x42;
	dev->com_event_log_count = 1;

	ASSERT_EQ(run(dev, fc11_extra, sizeof(fc11_extra), resp) == 2);
	ASSERT_EQ(resp[0] == (u8)(FC_GET_COM_EVENT_COUNTER | 0x80));
	ASSERT_EQ(resp[1] == EXC_ILLEGAL_DATA_VALUE);
	ASSERT_EQ(run(dev, fc12_extra, sizeof(fc12_extra), resp) == 2);
	ASSERT_EQ(resp[0] == (u8)(FC_GET_COM_EVENT_LOG | 0x80));
	ASSERT_EQ(resp[1] == EXC_ILLEGAL_DATA_VALUE);

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 9);
	ASSERT_EQ(resp[1] == 7 && resp[2] == 0xFF && resp[3] == 0xFF);
	ASSERT_EQ(resp[4] == 0x00 && resp[5] == 0x00); /* exceptions are not events */
	ASSERT_EQ(resp[6] == 0x00 && resp[7] == 0x03); /* both malformed PDUs counted */
	ASSERT_EQ(resp[8] == 0x42);
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

TEST(DiagnosticsReturnQueryDataEchoesArbitraryDataField)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { FC_DIAGNOSTICS, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(req));
	ASSERT_EQ(memcmp(resp, req, sizeof(req)) == 0);
	arena_release(arena);
}

TEST(DiagnosticsRejectsMalformedRequest)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		short_req[] = { FC_DIAGNOSTICS, 0x00, 0x00, 0x12 };
	u8		odd_data[]  = { FC_DIAGNOSTICS, 0x00, 0x00, 0x12, 0x34, 0x00 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, short_req, sizeof(short_req), resp) == 2);
	ASSERT_EQ(exception_response_is(resp, 2, EXC_ILLEGAL_DATA_VALUE));
	ASSERT_EQ(run(dev, odd_data, sizeof(odd_data), resp) == 2);
	ASSERT_EQ(exception_response_is(resp, 2, EXC_ILLEGAL_DATA_VALUE));
	arena_release(arena);
}

TEST(DiagnosticsUnsupportedSubfunctionIsIllegalFunction)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { FC_DIAGNOSTICS, 0x00, 0x13, 0x00, 0x00 };
	u8		vendor[] = { FC_DIAGNOSTICS, 0x00, 0x15, 0x00, 0x00 };
	u8		short_req[] = { FC_DIAGNOSTICS, 0x00, 0x13, 0x00 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 2);
	ASSERT_EQ(exception_response_is(resp, 2, EXC_ILLEGAL_FUNCTION));
	ASSERT_EQ(run(dev, vendor, sizeof(vendor), resp) == 2);
	ASSERT_EQ(exception_response_is(resp, 2, EXC_ILLEGAL_FUNCTION));
	ASSERT_EQ(run(dev, short_req, sizeof(short_req), resp) == 2);
	ASSERT_EQ(exception_response_is(resp, 2, EXC_ILLEGAL_FUNCTION));
	arena_release(arena);
}

TEST(DiagnosticsReservedDataWordMustBeZero)
{
	static const u16 subfunctions[] = {
		DIAG_SUB_READ_DIAGNOSTIC_REGISTER,
		DIAG_SUB_LISTEN_ONLY_MODE,
		DIAG_SUB_CLEAR_COUNTERS,
		DIAG_SUB_RETURN_BUS_MESSAGE_COUNT,
		DIAG_SUB_RETURN_BUS_COMM_ERROR_COUNT,
		DIAG_SUB_RETURN_BUS_EXCEPTION_ERROR_COUNT,
		DIAG_SUB_RETURN_SLAVE_MESSAGE_COUNT,
		DIAG_SUB_RETURN_SLAVE_NO_RESPONSE_COUNT,
		DIAG_SUB_RETURN_SLAVE_NAK_COUNT,
		DIAG_SUB_RETURN_SLAVE_BUSY_COUNT,
		DIAG_SUB_RETURN_BUS_CHAR_OVERRUN_COUNT,
		DIAG_SUB_CLEAR_OVERRUN_COUNTER_AND_FLAG,
	};
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[6];
	u8		resp[MODBUS_MAX_PDU];

	for (u32 i = 0; i < ARRAY_COUNT(subfunctions); i++)
	{
		diagnostic_request(req, subfunctions[i], 0x0001);
		ASSERT_EQ(run(dev, req, 5, resp) == 2);
		ASSERT_EQ(exception_response_is(resp, 2, EXC_ILLEGAL_DATA_VALUE));
	}
	ASSERT_EQ(!dev->listen_only);

	diagnostic_request(req, DIAG_SUB_RETURN_BUS_MESSAGE_COUNT, 0x0000);
	ASSERT_EQ(run(dev, req, 4, resp) == 2);
	ASSERT_EQ(exception_response_is(resp, 2, EXC_ILLEGAL_DATA_VALUE));
	ASSERT_EQ(run(dev, req, 6, resp) == 2);
	ASSERT_EQ(exception_response_is(resp, 2, EXC_ILLEGAL_DATA_VALUE));
	arena_release(arena);
}

TEST(DiagnosticsRestartCommunicationsClearsStateAndEchoes)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[5];
	u8		resp[MODBUS_MAX_PDU];

	dev->diagnostic_register    = DIAG_REG_CHARACTER_OVERRUN;
	dev->diag_comm_error_count  = 7;
	dev->diag_exception_count   = 8;
	dev->diag_no_response_count = 9;
	dev->diag_nak_count         = 4;
	dev->diag_busy_count        = 5;
	dev->diag_overrun_count     = 6;
	dev->com_event_count        = 11;

	diagnostic_request(req, DIAG_SUB_RESTART_COMMUNICATIONS, 0x0000);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(req));
	ASSERT_EQ(memcmp(resp, req, sizeof(req)) == 0);
	ASSERT_EQ(dev->diagnostic_register == 0);
	ASSERT_EQ(dev->message_count == 0);
	ASSERT_EQ(dev->diag_comm_error_count == 0 && dev->diag_exception_count == 0);
	ASSERT_EQ(dev->diag_no_response_count == 0 && dev->diag_nak_count == 0);
	ASSERT_EQ(dev->diag_busy_count == 0 && dev->diag_overrun_count == 0);
	ASSERT_EQ(dev->com_event_count == 0);

	dev->diagnostic_register    = DIAG_REG_CHARACTER_OVERRUN;
	dev->diag_comm_error_count  = 7;
	dev->diag_exception_count   = 8;
	dev->diag_no_response_count = 9;
	dev->diag_nak_count         = 4;
	dev->diag_busy_count        = 5;
	dev->diag_overrun_count     = 6;
	diagnostic_request(req, DIAG_SUB_RESTART_COMMUNICATIONS, 0xFF00);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(req));
	ASSERT_EQ(memcmp(resp, req, sizeof(req)) == 0);
	ASSERT_EQ(!dev->listen_only);
	ASSERT_EQ(dev->com_event_count == 0);
	ASSERT_EQ(dev->diagnostic_register == 0);
	ASSERT_EQ(dev->message_count == 0);
	ASSERT_EQ(dev->diag_comm_error_count == 0 && dev->diag_exception_count == 0);
	ASSERT_EQ(dev->diag_no_response_count == 0 && dev->diag_nak_count == 0);
	ASSERT_EQ(dev->diag_busy_count == 0 && dev->diag_overrun_count == 0);
	arena_release(arena);
}

TEST(DiagnosticsRestart0000ClearsCountersAndPreservesEventLog)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[5];
	u8		resp[MODBUS_MAX_PDU];
	u8		fc12[] = { FC_GET_COM_EVENT_LOG };
	static const u8	log[] = { 0x11, 0x22, 0x33 };

	dev->com_event_count        = 11;
	dev->com_event_log_count    = (u16)sizeof(log);
	dev->diag_comm_error_count  = 7;
	dev->diag_exception_count   = 8;
	dev->diag_no_response_count = 9;
	dev->diag_nak_count         = 4;
	dev->diag_busy_count        = 5;
	dev->diag_overrun_count     = 6;
	dev->message_count          = 13;
	memcpy(dev->com_events, log, sizeof(log));

	diagnostic_request(req, DIAG_SUB_RESTART_COMMUNICATIONS, 0x0000);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(req));
	ASSERT_EQ(memcmp(resp, req, sizeof(req)) == 0);
	ASSERT_EQ(dev->com_event_count == 0);
	ASSERT_EQ(dev->message_count == 0);
	ASSERT_EQ(dev->diag_comm_error_count == 0 && dev->diag_exception_count == 0);
	ASSERT_EQ(dev->diag_no_response_count == 0 && dev->diag_nak_count == 0);
	ASSERT_EQ(dev->diag_busy_count == 0 && dev->diag_overrun_count == 0);
	ASSERT_EQ(dev->com_event_log_count == sizeof(log));
	ASSERT_EQ(memcmp(dev->com_events, log, sizeof(log)) == 0);

	ASSERT_EQ(run(dev, fc12, sizeof(fc12), resp) == 8 + sizeof(log));
	ASSERT_EQ(resp[1] == 6 + sizeof(log));
	ASSERT_EQ(resp[4] == 0 && resp[5] == 0);
	ASSERT_EQ(resp[6] == 0 && resp[7] == 1);
	ASSERT_EQ(memcmp(resp + 8, log, sizeof(log)) == 0);
	arena_release(arena);
}

TEST(DiagnosticsRestartFF00ClearsCountersAndEventLog)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[5];
	u8		resp[MODBUS_MAX_PDU];
	u8		fc12[] = { FC_GET_COM_EVENT_LOG };
	static const u8	log[] = { 0x11, 0x22, 0x33 };

	dev->com_event_count        = 11;
	dev->com_event_log_count    = (u16)sizeof(log);
	dev->diag_comm_error_count  = 7;
	dev->diag_exception_count   = 8;
	dev->diag_no_response_count = 9;
	dev->diag_nak_count         = 4;
	dev->diag_busy_count        = 5;
	dev->diag_overrun_count     = 6;
	dev->message_count          = 13;
	memcpy(dev->com_events, log, sizeof(log));

	diagnostic_request(req, DIAG_SUB_RESTART_COMMUNICATIONS, 0xFF00);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(req));
	ASSERT_EQ(memcmp(resp, req, sizeof(req)) == 0);
	ASSERT_EQ(dev->com_event_count == 0);
	ASSERT_EQ(dev->message_count == 0);
	ASSERT_EQ(dev->diag_comm_error_count == 0 && dev->diag_exception_count == 0);
	ASSERT_EQ(dev->diag_no_response_count == 0 && dev->diag_nak_count == 0);
	ASSERT_EQ(dev->diag_busy_count == 0 && dev->diag_overrun_count == 0);
	ASSERT_EQ(dev->com_event_log_count == 0);
	for (u32 i = 0; i < DEVICE_MAX_COM_EVENTS; i++)
		ASSERT_EQ(dev->com_events[i] == 0);

	ASSERT_EQ(run(dev, fc12, sizeof(fc12), resp) == 8);
	ASSERT_EQ(resp[1] == 6);
	ASSERT_EQ(resp[4] == 0 && resp[5] == 0);
	ASSERT_EQ(resp[6] == 0 && resp[7] == 1);
	arena_release(arena);
}

TEST(DiagnosticsListenOnlyMonitorsMessageCountWithoutResponses)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			req[5];
	u8			resp[MODBUS_MAX_PDU];
	u8			read[] = { FC_READ_HOLDING_REGISTERS, 0, 0, 0, 1 };
	u16			count  = 0;

	rng_seed(&rng, 1);
	diagnostic_request(req, DIAG_SUB_LISTEN_ONLY_MODE, 0x0000);
	ASSERT_EQ(run_request(dev, &rng, req, sizeof(req), resp, &decision) == 0);
	ASSERT_EQ(decision.drop);
	ASSERT_EQ(dev->listen_only);
	count = dev->message_count;

	decision = (fault_decision_t){ 0 };
	ASSERT_EQ(run_request(dev, &rng, read, sizeof(read), resp, &decision) == 0);
	ASSERT_EQ(decision.drop);
	ASSERT_EQ(dev->message_count == (u16)(count + 1));
	ASSERT_EQ(dev->com_event_count == 0);
	ASSERT_EQ(dev->diag_exception_count == 0);

	decision = (fault_decision_t){ 0 };
	diagnostic_request(req, DIAG_SUB_RETURN_BUS_MESSAGE_COUNT, 0x0000);
	ASSERT_EQ(run_request(dev, &rng, req, sizeof(req), resp, &decision) == 0);
	ASSERT_EQ(decision.drop);
	ASSERT_EQ(dev->message_count == (u16)(count + 2));

	decision = (fault_decision_t){ 0 };
	diagnostic_request(req, DIAG_SUB_RETURN_SLAVE_MESSAGE_COUNT, 0x0000);
	ASSERT_EQ(run_request(dev, &rng, req, sizeof(req), resp, &decision) == 0);
	ASSERT_EQ(decision.drop);
	ASSERT_EQ(dev->message_count == (u16)(count + 3));
	ASSERT_EQ(dev->diag_no_response_count == 4);

	/* Read the counters only after leaving listen-only; a restart would reset them. */
	dev->listen_only = false;
	diagnostic_request(req, DIAG_SUB_RETURN_BUS_MESSAGE_COUNT, 0x0000);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(get_be16(resp + 3) == (u16)(count + 4));
	diagnostic_request(req, DIAG_SUB_RETURN_SLAVE_MESSAGE_COUNT, 0x0000);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(get_be16(resp + 3) == (u16)(count + 5));
	arena_release(arena);
}

TEST(DiagnosticsRestartFromListenOnlyIsProcessedWithoutResponse)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			req[5];
	u8			resp[MODBUS_MAX_PDU];
	u8			read[] = { FC_READ_HOLDING_REGISTERS, 0, 0, 0, 1 };
	u16			message_count = 0;
	u16			exception_count = 0;

	rng_seed(&rng, 1);
	diagnostic_request(req, DIAG_SUB_LISTEN_ONLY_MODE, 0x0000);
	ASSERT_EQ(run_request(dev, &rng, req, sizeof(req), resp, &decision) == 0);
	ASSERT_EQ(decision.drop);
	ASSERT_EQ(dev->listen_only);
	ASSERT_EQ(dev->diag_no_response_count == 1);

	message_count   = dev->message_count;
	exception_count = dev->diag_exception_count;
	decision        = (fault_decision_t){ 0 };
	ASSERT_EQ(run_request(dev, &rng, read, sizeof(read), resp, &decision) == 0);
	ASSERT_EQ(decision.drop);
	ASSERT_EQ(dev->message_count == (u16)(message_count + 1));
	ASSERT_EQ(dev->diag_exception_count == exception_count);
	ASSERT_EQ(dev->diag_no_response_count == 2);

	decision = (fault_decision_t){ 0 };
	diagnostic_request(req, DIAG_SUB_RESTART_COMMUNICATIONS, 0x0000);
	ASSERT_EQ(run_request(dev, &rng, req, sizeof(req), resp, &decision) == 0);
	ASSERT_EQ(decision.drop);
	ASSERT_EQ(!dev->listen_only);
	ASSERT_EQ(dev->message_count == 0);
	ASSERT_EQ(dev->diag_no_response_count == 1);

	decision = (fault_decision_t){ 0 };
	ASSERT_EQ(run_request(dev, &rng, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(!decision.drop);
	ASSERT_EQ(dev->diag_no_response_count == 1);

	decision = (fault_decision_t){ 0 };
	diagnostic_request(req, DIAG_SUB_LISTEN_ONLY_MODE, 0x0000);
	ASSERT_EQ(run_request(dev, &rng, req, sizeof(req), resp, &decision) == 0);
	ASSERT_EQ(decision.drop);
	ASSERT_EQ(dev->diag_no_response_count == 2);

	decision = (fault_decision_t){ 0 };
	diagnostic_request(req, DIAG_SUB_RESTART_COMMUNICATIONS, 0xFF00);
	ASSERT_EQ(run_request(dev, &rng, req, sizeof(req), resp, &decision) == 0);
	ASSERT_EQ(decision.drop);
	ASSERT_EQ(!dev->listen_only);
	ASSERT_EQ(dev->com_event_count == 0);
	ASSERT_EQ(dev->diag_no_response_count == 1);

	decision = (fault_decision_t){ 0 };
	ASSERT_EQ(run_request(dev, &rng, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(!decision.drop);
	arena_release(arena);
}

TEST(DiagnosticsRestartCommunicationsRejectsInvalidShapeAndValue)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[6];
	u8		resp[MODBUS_MAX_PDU];

	diagnostic_request(req, DIAG_SUB_RESTART_COMMUNICATIONS, 0x1234);
	ASSERT_EQ(run(dev, req, 5, resp) == 2);
	ASSERT_EQ(exception_response_is(resp, 2, EXC_ILLEGAL_DATA_VALUE));
	ASSERT_EQ(!dev->listen_only && dev->diagnostic_register == 0);

	diagnostic_request(req, DIAG_SUB_RESTART_COMMUNICATIONS, 0x0000);
	ASSERT_EQ(run(dev, req, 4, resp) == 2);
	ASSERT_EQ(exception_response_is(resp, 2, EXC_ILLEGAL_DATA_VALUE));
	ASSERT_EQ(run(dev, req, 6, resp) == 2);
	ASSERT_EQ(exception_response_is(resp, 2, EXC_ILLEGAL_DATA_VALUE));
	arena_release(arena);
}

TEST(DiagnosticsChangeAsciiDelimiterIsIllegalDataValue)
{
	static const u16	values[] = { 0x0000, 0x0023, 0x2300, 0x2323 };
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	u8			req[6];
	u8			resp[MODBUS_MAX_PDU];

	diagnostic_request(req, DIAG_SUB_READ_DIAGNOSTIC_REGISTER, 0x0000);
	ASSERT_EQ(run(dev, req, 5, resp) == 5);
	ASSERT_EQ(get_be16(resp + 1) == DIAG_SUB_READ_DIAGNOSTIC_REGISTER);
	ASSERT_EQ(get_be16(resp + 3) == 0);

	for (u32 i = 0; i < ARRAY_COUNT(values); i++)
	{
		diagnostic_request(req, DIAG_SUB_CHANGE_ASCII_DELIMITER, values[i]);
		ASSERT_EQ(run(dev, req, 5, resp) == 2);
		ASSERT_EQ(exception_response_is(resp, 2, EXC_ILLEGAL_DATA_VALUE));
		ASSERT_EQ(dev->diagnostic_register == 0);
	}

	diagnostic_request(req, DIAG_SUB_CHANGE_ASCII_DELIMITER, 0x2300);
	ASSERT_EQ(run(dev, req, 3, resp) == 2);
	ASSERT_EQ(exception_response_is(resp, 2, EXC_ILLEGAL_DATA_VALUE));
	ASSERT_EQ(run(dev, req, 6, resp) == 2);
	ASSERT_EQ(exception_response_is(resp, 2, EXC_ILLEGAL_DATA_VALUE));

	diagnostic_request(req, DIAG_SUB_READ_DIAGNOSTIC_REGISTER, 0x0000);
	ASSERT_EQ(run(dev, req, 5, resp) == 5);
	ASSERT_EQ(get_be16(resp + 3) == 0);
	arena_release(arena);
}

TEST(DiagnosticsClearOverrunCounterAndFlag)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[5];
	u8		resp[MODBUS_MAX_PDU];

	dev->diag_overrun_count  = 5;
	dev->diagnostic_register = DIAG_REG_CHARACTER_OVERRUN;
	diagnostic_request(req, DIAG_SUB_RETURN_BUS_CHAR_OVERRUN_COUNT, 0x0000);
	ASSERT_EQ(run(dev, req, 5, resp) == 5);
	ASSERT_EQ(get_be16(resp + 3) == 5);

	diagnostic_request(req, DIAG_SUB_CLEAR_OVERRUN_COUNTER_AND_FLAG, 0x0000);
	ASSERT_EQ(run(dev, req, 5, resp) == 5);
	ASSERT_EQ(memcmp(resp, req, 5) == 0);
	ASSERT_EQ(dev->diag_overrun_count == 0);
	ASSERT_EQ(dev->diagnostic_register == 0);

	diagnostic_request(req, DIAG_SUB_RETURN_BUS_CHAR_OVERRUN_COUNT, 0x0000);
	ASSERT_EQ(run(dev, req, 5, resp) == 5);
	ASSERT_EQ(get_be16(resp + 3) == 0);

	diagnostic_request(req, DIAG_SUB_CLEAR_OVERRUN_COUNTER_AND_FLAG, 0x0001);
	ASSERT_EQ(run(dev, req, 5, resp) == 2);
	ASSERT_EQ(exception_response_is(resp, 2, EXC_ILLEGAL_DATA_VALUE));
	arena_release(arena);
}

TEST(DiagnosticsClearCountersResetsAllDiagnostics)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[5];
	u8		resp[MODBUS_MAX_PDU];

	dev->listen_only            = false;
	dev->diagnostic_register    = DIAG_REG_CHARACTER_OVERRUN;
	dev->diag_comm_error_count  = 7;
	dev->diag_exception_count   = 8;
	dev->diag_no_response_count = 9;
	dev->diag_nak_count         = 4;
	dev->diag_busy_count        = 5;
	dev->diag_overrun_count     = 6;
	dev->com_event_count        = 11;

	diagnostic_request(req, DIAG_SUB_CLEAR_COUNTERS, 0x0000);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(req));
	ASSERT_EQ(memcmp(resp, req, sizeof(req)) == 0);
	ASSERT_EQ(!dev->listen_only);
	ASSERT_EQ(dev->diagnostic_register == 0);
	ASSERT_EQ(dev->message_count == 0);
	ASSERT_EQ(dev->diag_comm_error_count == 0 && dev->diag_exception_count == 0);
	ASSERT_EQ(dev->diag_no_response_count == 0 && dev->diag_nak_count == 0);
	ASSERT_EQ(dev->diag_busy_count == 0 && dev->diag_overrun_count == 0);
	ASSERT_EQ(dev->com_event_count == 12);
	arena_release(arena);
}

TEST(DiagnosticsCounterSubfunctionsReflectRecordedEvents)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	rng_t		rng;
	fault_decision_t	decision = { 0 };
	u8		req[5];
	u8		resp[MODBUS_MAX_PDU];
	u8		bad_fc[]    = { 0x99 };
	u8		bad_sub[]   = { FC_DIAGNOSTICS, 0x00, 0x13, 0x00, 0x00 };
	u8		bad_addr[]  = { FC_READ_HOLDING_REGISTERS, 0x7F, 0xFF, 0x00, 0x01 };
	u16		expected    = 0;

	rng_seed(&rng, 1);
	ASSERT_EQ(run(dev, bad_fc, sizeof(bad_fc), resp) == 2);
	ASSERT_EQ(resp[1] == EXC_ILLEGAL_FUNCTION);
	ASSERT_EQ(run(dev, bad_sub, sizeof(bad_sub), resp) == 2);
	ASSERT_EQ(resp[1] == EXC_ILLEGAL_FUNCTION);
	ASSERT_EQ(run(dev, bad_addr, sizeof(bad_addr), resp) == 2);
	ASSERT_EQ(resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	diagnostic_request(req, DIAG_SUB_READ_DIAGNOSTIC_REGISTER, 0x0000);
	ASSERT_EQ(run(dev, req, 4, resp) == 2);
	ASSERT_EQ(resp[1] == EXC_ILLEGAL_DATA_VALUE);

	diagnostic_request(req, DIAG_SUB_LISTEN_ONLY_MODE, 0x0000);
	ASSERT_EQ(run_request(dev, &rng, req, 5, resp, &decision) == 0);
	ASSERT_EQ(decision.drop);
	dev->listen_only = false; /* test-only: allow counter responses to be read */

	diagnostic_request(req, DIAG_SUB_RETURN_BUS_MESSAGE_COUNT, 0x0000);
	expected = (u16)(dev->message_count + 1);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(get_be16(resp + 1) == DIAG_SUB_RETURN_BUS_MESSAGE_COUNT);
	ASSERT_EQ(get_be16(resp + 3) == expected && dev->message_count == expected);

	diagnostic_request(req, DIAG_SUB_RETURN_BUS_COMM_ERROR_COUNT, 0x0000);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(get_be16(resp + 3) == 0);

	diagnostic_request(req, DIAG_SUB_RETURN_BUS_EXCEPTION_ERROR_COUNT, 0x0000);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(get_be16(resp + 3) == 4);

	diagnostic_request(req, DIAG_SUB_RETURN_SLAVE_MESSAGE_COUNT, 0x0000);
	expected = (u16)(dev->message_count + 1);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(get_be16(resp + 1) == DIAG_SUB_RETURN_SLAVE_MESSAGE_COUNT);
	ASSERT_EQ(get_be16(resp + 3) == expected && dev->message_count == expected);

	diagnostic_request(req, DIAG_SUB_RETURN_SLAVE_NO_RESPONSE_COUNT, 0x0000);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(get_be16(resp + 3) == 1);

	diagnostic_request(req, DIAG_SUB_RETURN_SLAVE_NAK_COUNT, 0x0000);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(get_be16(resp + 3) == 0);

	diagnostic_request(req, DIAG_SUB_RETURN_SLAVE_BUSY_COUNT, 0x0000);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(get_be16(resp + 3) == 0);

	diagnostic_request(req, DIAG_SUB_RETURN_BUS_CHAR_OVERRUN_COUNT, 0x0000);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(get_be16(resp + 3) == 0);
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
	u8		bad_range[] = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x27, 0x0F, 0x00, 0x02 };
	u8		bad_file[]  = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
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
	run(dev, bad_range, sizeof(bad_range), resp);
	ASSERT_EQ(resp[0] == 0x94 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_file, sizeof(bad_file), resp);
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

TEST(ReadFileRecordMultipleSubRequestsExceedPduSize)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = device_new(arena, 0);
	u16		data[126];
	u8		req[2 + 7 * 35];
	u8		resp[MODBUS_MAX_PDU];

	for (u32 i = 0; i < 126; i++)
		data[i] = (u16)i;
	ASSERT_EQ(device_add_file_record(dev, 1, 0, 126, data) == NULL);

	/* 35 sub-requests * (2-byte sub-resp header + 2*3 data bytes) = 280 bytes,
	 * plus FC + resp-data-length = 282, which exceeds MODBUS_MAX_PDU. */
	req[0] = FC_READ_FILE_RECORD;
	req[1] = 7 * 35;
	for (u32 i = 0; i < 35; i++)
	{
		u8* sub = req + 2 + i * 7;

		sub[0] = 0x06;
		put_be16(sub + 1, 1);
		put_be16(sub + 3, 0);
		put_be16(sub + 5, 3);
	}

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 2);
	ASSERT_EQ(resp[0] == 0x94 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	arena_release(arena);
}

TEST(WriteFileRecordSingleSubRequestEchoesAndUpdates)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_file_device(arena);
	u8		req[]  = { FC_WRITE_FILE_RECORD, 0x0B, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0xAA, 0xAA, 0xBB, 0xBB };
	u8		read[] = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02 };
	u8		resp[MODBUS_MAX_PDU];
	u8		want[] = { FC_READ_FILE_RECORD, 0x06, 0x05, 0x06, 0xAA, 0xAA, 0xBB, 0xBB };

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(req));
	ASSERT_EQ(memcmp(resp, req, sizeof(req)) == 0);
	ASSERT_EQ(run(dev, read, sizeof(read), resp) == sizeof(want));
	ASSERT_EQ(memcmp(resp, want, sizeof(want)) == 0);
	arena_release(arena);
}

TEST(WriteFileRecordMultipleSubRequests)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_file_device(arena);
	u8		req[]  = {
		FC_WRITE_FILE_RECORD, 0x12,
		0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x11,	/* file 1, rec 0 */
		0x06, 0x00, 0x04, 0x00, 0x02, 0x00, 0x01, 0x00, 0x22,	/* file 4, rec 2 */
	};
	u8		read1[] = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02 };
	u8		read4[] = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x04, 0x00, 0x02, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(req));
	ASSERT_EQ(memcmp(resp, req, sizeof(req)) == 0);

	run(dev, read1, sizeof(read1), resp);
	ASSERT_EQ(resp[0] == FC_READ_FILE_RECORD && resp[4] == 0x00 && resp[5] == 0x11 && resp[6] == 0x56 && resp[7] == 0x78);
	run(dev, read4, sizeof(read4), resp);
	ASSERT_EQ(resp[0] == FC_READ_FILE_RECORD && resp[4] == 0x00 && resp[5] == 0x22);
	arena_release(arena);
}

TEST(WriteFileRecordRejectsMalformedRequest)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_file_device(arena);
	u8		short_req[] = { FC_WRITE_FILE_RECORD };
	u8		bad_count[] = { FC_WRITE_FILE_RECORD, 0x08, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01 }; /* byte count does not match PDU */
	u8		bad_low[]   = { FC_WRITE_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01 }; /* byte count below FC21 minimum */
	u8		bad_high[]  = { FC_WRITE_FILE_RECORD, 0xFC, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01 }; /* byte count above FC21 maximum */
	u8		bad_len[]   = { FC_WRITE_FILE_RECORD, 0x09, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00 }; /* PDU longer than byte count */
	u8		bad_ref[]   = { FC_WRITE_FILE_RECORD, 0x09, 0x05, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01 };
	u8		bad_rlen[]  = { FC_WRITE_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 };
	u8		bad_rec[]   = { FC_WRITE_FILE_RECORD, 0x09, 0x06, 0x00, 0x01, 0x27, 0x10, 0x00, 0x01, 0x00, 0x01 };
	u8		bad_range[] = { FC_WRITE_FILE_RECORD, 0x0B, 0x06, 0x00, 0x01, 0x27, 0x0F, 0x00, 0x02, 0x00, 0x01, 0x00, 0x02 }; /* range exceeds 0x270F */
	u8		bad_file[]  = { FC_WRITE_FILE_RECORD, 0x09, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01 };
	u8		trunc[]     = { FC_WRITE_FILE_RECORD, 0x09, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01 }; /* claims 2 registers but only 1 provided */
	u8		misaligned[]= { FC_WRITE_FILE_RECORD, 0x0A, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00 }; /* trailing byte after complete sub-request */
	u8		resp[MODBUS_MAX_PDU];

	run(dev, short_req, sizeof(short_req), resp);
	ASSERT_EQ(resp[0] == 0x95 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_count, sizeof(bad_count), resp);
	ASSERT_EQ(resp[0] == 0x95 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_low, sizeof(bad_low), resp);
	ASSERT_EQ(resp[0] == 0x95 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_high, sizeof(bad_high), resp);
	ASSERT_EQ(resp[0] == 0x95 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_len, sizeof(bad_len), resp);
	ASSERT_EQ(resp[0] == 0x95 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_ref, sizeof(bad_ref), resp);
	ASSERT_EQ(resp[0] == 0x95 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_rlen, sizeof(bad_rlen), resp);
	ASSERT_EQ(resp[0] == 0x95 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_rec, sizeof(bad_rec), resp);
	ASSERT_EQ(resp[0] == 0x95 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_range, sizeof(bad_range), resp);
	ASSERT_EQ(resp[0] == 0x95 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_file, sizeof(bad_file), resp);
	ASSERT_EQ(resp[0] == 0x95 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, trunc, sizeof(trunc), resp);
	ASSERT_EQ(resp[0] == 0x95 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, misaligned, sizeof(misaligned), resp);
	ASSERT_EQ(resp[0] == 0x95 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	arena_release(arena);
}

TEST(WriteFileRecordUnavailableIsAddressError)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_file_device(arena);
	u8		missing_file[] = { FC_WRITE_FILE_RECORD, 0x09, 0x06, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01 };
	u8		missing_rec[]  = { FC_WRITE_FILE_RECORD, 0x09, 0x06, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01 };
	u8		gap[]          = { FC_WRITE_FILE_RECORD, 0x0D, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03 };
	u8		resp[MODBUS_MAX_PDU];

	run(dev, missing_file, sizeof(missing_file), resp);
	ASSERT_EQ(resp[0] == 0x95 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	run(dev, missing_rec, sizeof(missing_rec), resp);
	ASSERT_EQ(resp[0] == 0x95 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	run(dev, gap, sizeof(gap), resp);
	ASSERT_EQ(resp[0] == 0x95 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	arena_release(arena);
}

TEST(WriteFileRecordIsAllOrNothing)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_file_device(arena);
	u8		req[]  = {
		FC_WRITE_FILE_RECORD, 0x12,
		0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x77,	/* valid */
		0x06, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01, 0x00, 0x88,	/* unavailable file */
	};
	u8		read[] = { FC_READ_FILE_RECORD, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];

	run(dev, req, sizeof(req), resp);
	ASSERT_EQ(resp[0] == 0x95 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	run(dev, read, sizeof(read), resp);
	ASSERT_EQ(resp[0] == FC_READ_FILE_RECORD && resp[4] == 0x12 && resp[5] == 0x34);
	arena_release(arena);
}

TEST(WriteFileRecordUnavailableWhenNoFileRecordsConfigured)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { FC_WRITE_FILE_RECORD, 0x09, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];

	run(dev, req, sizeof(req), resp);
	ASSERT_EQ(resp[0] == 0x95 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	arena_release(arena);
}

TEST(MaskWriteRegisterAppliesFormulaAndEchoes)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { FC_MASK_WRITE_REGISTER, 0x00, 0x00, 0xF2, 0x02, 0x00, 0x25 };
	u8		read[] = { 0x03, 0x00, 0x00, 0x00, 0x01 };
	u8		resp[MODBUS_MAX_PDU];

	/* holding[0] starts at 0x1234: (0x1234 & 0xF202) | (0x0025 & ~0xF202) = 0x1225 */
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(req));
	ASSERT_EQ(memcmp(resp, req, sizeof(req)) == 0);
	ASSERT_EQ(run(dev, read, sizeof(read), resp) == 4);
	ASSERT_EQ(resp[2] == 0x12 && resp[3] == 0x25);
	arena_release(arena);
}

TEST(MaskWriteRegisterRejectsMalformedRequest)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		short_req[] = { FC_MASK_WRITE_REGISTER, 0x00, 0x00, 0xFF, 0xFF, 0x00 };
	u8		extra[]     = { FC_MASK_WRITE_REGISTER, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, short_req, sizeof(short_req), resp) == 2);
	ASSERT_EQ(resp[0] == 0x96 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	ASSERT_EQ(run(dev, extra, sizeof(extra), resp) == 2);
	ASSERT_EQ(resp[0] == 0x96 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	arena_release(arena);
}

TEST(MaskWriteRegisterUnavailableAddressIsAddressError)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		missing[]  = { FC_MASK_WRITE_REGISTER, 0x00, 0x05, 0xFF, 0xFF, 0x00, 0x00 };
	u8		readonly[] = { FC_MASK_WRITE_REGISTER, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00 };
	u8		resp[MODBUS_MAX_PDU];
	point_t		p = { 0 };

	run(dev, missing, sizeof(missing), resp);
	ASSERT_EQ(resp[0] == 0x96 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);

	/* input register 0 exists, but holding register 0 is not configured here */
	dev = make_device(arena);
	dev->point_count = 0;
	memset(&dev->spaces[SPACE_HOLDING], 0, sizeof(dev->spaces[SPACE_HOLDING]));
	p.kind = POINT_STATIC;
	p.space = SPACE_INPUT;
	p.type = VT_U16;
	p.address = 0;
	p.static_value = 7;
	device_add_point(dev, &p);
	run(dev, readonly, sizeof(readonly), resp);
	ASSERT_EQ(resp[0] == 0x96 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	arena_release(arena);
}

TEST(ReadWriteMultipleRegistersSuccessAndReadback)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = {
		FC_READ_WRITE_MULTIPLE_REGISTERS,
		0x00, 0x00, 0x00, 0x02,	/* read addr 0, qty 2 */
		0x00, 0x00, 0x00, 0x02,	/* write addr 0, qty 2 */
		0x04,			/* byte count */
		0xAA, 0xAA, 0xBB, 0xBB,	/* write values */
	};
	u8		read[] = { 0x03, 0x00, 0x00, 0x00, 0x02 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 6);
	ASSERT_EQ(resp[0] == FC_READ_WRITE_MULTIPLE_REGISTERS && resp[1] == 0x04);
	ASSERT_EQ(resp[2] == 0xAA && resp[3] == 0xAA && resp[4] == 0xBB && resp[5] == 0xBB);

	ASSERT_EQ(run(dev, read, sizeof(read), resp) == 6);
	ASSERT_EQ(resp[2] == 0xAA && resp[3] == 0xAA && resp[4] == 0xBB && resp[5] == 0xBB);
	arena_release(arena);
}

TEST(ReadWriteMultipleRegistersWriteBeforeReadWithOverlap)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = device_new(arena, 0);
	point_t		p     = { 0 };
	u8		req[]  = {
		FC_READ_WRITE_MULTIPLE_REGISTERS,
		0x00, 0x01, 0x00, 0x03,	/* read addr 1, qty 3 */
		0x00, 0x00, 0x00, 0x03,	/* write addr 0, qty 3 */
		0x06,
		0x00, 0x64, 0x00, 0x65, 0x00, 0x66,	/* 100,101,102 */
	};
	u8		resp[MODBUS_MAX_PDU];
	u16		out[4] = { 0, 0, 0, 0 };

	p.kind = POINT_STATIC;
	p.space = SPACE_HOLDING;
	p.type = VT_U16;
	for (u32 i = 0; i < 4; i++) { p.address = i; p.static_value = 10 + i; device_add_point(dev, &p); }

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 8);
	ASSERT_EQ(resp[0] == FC_READ_WRITE_MULTIPLE_REGISTERS && resp[1] == 0x06);
	/* read of 1,2,3 after write to 0,1,2 => 101,102,13 */
	ASSERT_EQ(resp[2] == 0x00 && resp[3] == 0x65);
	ASSERT_EQ(resp[4] == 0x00 && resp[5] == 0x66);
	ASSERT_EQ(resp[6] == 0x00 && resp[7] == 0x0D);

	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 4, out));
	ASSERT_EQ(out[0] == 100 && out[1] == 101 && out[2] == 102 && out[3] == 13);
	arena_release(arena);
}

TEST(ReadWriteMultipleRegistersWriteBeforeReadNonOverlapping)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = device_new(arena, 0);
	point_t		p     = { 0 };
	u8		req[]  = {
		FC_READ_WRITE_MULTIPLE_REGISTERS,
		0x00, 0x02, 0x00, 0x02,	/* read addr 2, qty 2 */
		0x00, 0x00, 0x00, 0x02,	/* write addr 0, qty 2 */
		0x04,
		0x00, 0x0A, 0x00, 0x14,	/* 10,20 */
	};
	u8		resp[MODBUS_MAX_PDU];
	u16		out[4] = { 0, 0, 0, 0 };

	p.kind = POINT_STATIC;
	p.space = SPACE_HOLDING;
	p.type = VT_U16;
	for (u32 i = 0; i < 4; i++) { p.address = i; p.static_value = (f64)(i + 1); device_add_point(dev, &p); }

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 6);
	ASSERT_EQ(resp[0] == FC_READ_WRITE_MULTIPLE_REGISTERS && resp[1] == 0x04);
	/* Read returns post-write state for non-overlapping ranges. */
	ASSERT_EQ(get_be16(resp + 2) == 3 && get_be16(resp + 4) == 4);

	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 4, out));
	ASSERT_EQ(out[0] == 10 && out[1] == 20 && out[2] == 3 && out[3] == 4);
	arena_release(arena);
}

TEST(ReadWriteMultipleRegistersRejectsMalformedRequest)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		resp[MODBUS_MAX_PDU];
	u8		short_req[] = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01 };
	u8		bad_rqty0[] = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x01 };
	u8		bad_rqty[]  = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x01 };
	u8		bad_wqty0[] = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
	u8		bad_wqty[]  = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x7A, 0xF4 };
	u8		bad_bc[]    = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x00, 0x01, 0x00, 0x00 };
	u8		bad_len[]   = { FC_READ_WRITE_MULTIPLE_REGISTERS, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x01, 0x00 };

	run(dev, short_req, sizeof(short_req), resp);
	ASSERT_EQ(resp[0] == 0x97 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_rqty0, sizeof(bad_rqty0), resp);
	ASSERT_EQ(resp[0] == 0x97 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_rqty, sizeof(bad_rqty), resp);
	ASSERT_EQ(resp[0] == 0x97 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_wqty0, sizeof(bad_wqty0), resp);
	ASSERT_EQ(resp[0] == 0x97 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_wqty, sizeof(bad_wqty), resp);
	ASSERT_EQ(resp[0] == 0x97 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_bc, sizeof(bad_bc), resp);
	ASSERT_EQ(resp[0] == 0x97 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_len, sizeof(bad_len), resp);
	ASSERT_EQ(resp[0] == 0x97 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	arena_release(arena);
}

TEST(ReadWriteMultipleRegistersUnavailableAddressIsAllOrNothing)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = device_new(arena, 0);
	point_t		p     = { 0 };
	u8		resp[MODBUS_MAX_PDU];
	u16		out[2] = { 0, 0 };
	u8		bad_read[] = {
		FC_READ_WRITE_MULTIPLE_REGISTERS,
		0x00, 0x01, 0x00, 0x02,	/* read addr 1, qty 2 (addr 2 undefined) */
		0x00, 0x00, 0x00, 0x01,	/* write addr 0, qty 1 */
		0x02, 0x00, 0x63,
	};
	u8		bad_write[] = {
		FC_READ_WRITE_MULTIPLE_REGISTERS,
		0x00, 0x00, 0x00, 0x01,	/* read addr 0, qty 1 */
		0x00, 0x00, 0x00, 0x02,	/* write addr 0, qty 2 (addr 1 undefined for holding?) */
		0x04, 0x00, 0x63, 0x00, 0x64,
	};

	p.kind = POINT_STATIC;
	p.space = SPACE_HOLDING;
	p.type = VT_U16;
	p.address = 0; p.static_value = 10; device_add_point(dev, &p);
	p.address = 1; p.static_value = 20; device_add_point(dev, &p);

	run(dev, bad_read, sizeof(bad_read), resp);
	ASSERT_EQ(resp[0] == 0x97 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 2, out));
	ASSERT_EQ(out[0] == 10 && out[1] == 20);

	/* Rebuild device so holding 1 is undefined: keep only holding 0 and add holding 2 separately */
	dev = device_new(arena, 0);
	p.address = 0; p.static_value = 10; device_add_point(dev, &p);
	p.address = 2; p.static_value = 30; device_add_point(dev, &p);

	run(dev, bad_write, sizeof(bad_write), resp);
	ASSERT_EQ(resp[0] == 0x97 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 1, out));
	ASSERT_EQ(out[0] == 10);
	out[0] = 0;
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 2, 1, out));
	ASSERT_EQ(out[0] == 30);
	arena_release(arena);
}

TEST(ReadWriteMultipleRegistersRejectsNonHoldingTarget)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = device_new(arena, 0);
	point_t		p     = { 0 };
	u8		req[]  = {
		FC_READ_WRITE_MULTIPLE_REGISTERS,
		0x00, 0x00, 0x00, 0x01,
		0x00, 0x00, 0x00, 0x01,
		0x02, 0x00, 0x01,
	};
	u8		resp[MODBUS_MAX_PDU];

	p.kind = POINT_STATIC;
	p.space = SPACE_INPUT;
	p.type = VT_U16;
	p.address = 0;
	p.static_value = 7;
	device_add_point(dev, &p);

	run(dev, req, sizeof(req), resp);
	ASSERT_EQ(resp[0] == 0x97 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	arena_release(arena);
}

TEST(ReadFifoQueueReturnsConfiguredValues)
{
	arena_t*	arena  = ARENA_ALLOC();
	device_t*	dev    = device_new(arena, 0);
	u16		data[] = { 0x1234, 0x5678, 0x0003 };
	u8		req[]  = { FC_READ_FIFO_QUEUE, 0x00, 0x0A };
	u8		resp[MODBUS_MAX_PDU];
	u8		want[] = { FC_READ_FIFO_QUEUE, 0x00, 0x08, 0x00, 0x03, 0x12, 0x34, 0x56, 0x78, 0x00, 0x03 };

	ASSERT_EQ(device_add_fifo(dev, 10, 3, data) == NULL);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(want));
	ASSERT_EQ(memcmp(resp, want, sizeof(want)) == 0);
	arena_release(arena);
}

TEST(ReadFifoQueueEmptyAndNonDestructive)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = device_new(arena, 0);
	u16		data[] = { 0x0001, 0x0002 };
	u8		req1[] = { FC_READ_FIFO_QUEUE, 0x00, 0x01 };
	u8		req2[] = { FC_READ_FIFO_QUEUE, 0x00, 0x02 };
	u8		resp[MODBUS_MAX_PDU];
	u8		empty[] = { FC_READ_FIFO_QUEUE, 0x00, 0x02, 0x00, 0x00 };
	u8		want[]  = { FC_READ_FIFO_QUEUE, 0x00, 0x06, 0x00, 0x02, 0x00, 0x01, 0x00, 0x02 };

	ASSERT_EQ(device_add_fifo(dev, 1, 0, data) == NULL);
	ASSERT_EQ(device_add_fifo(dev, 2, 2, data) == NULL);

	ASSERT_EQ(run(dev, req1, sizeof(req1), resp) == sizeof(empty));
	ASSERT_EQ(memcmp(resp, empty, sizeof(empty)) == 0);

	ASSERT_EQ(run(dev, req2, sizeof(req2), resp) == sizeof(want));
	ASSERT_EQ(memcmp(resp, want, sizeof(want)) == 0);
	ASSERT_EQ(run(dev, req2, sizeof(req2), resp) == sizeof(want));
	ASSERT_EQ(memcmp(resp, want, sizeof(want)) == 0);
	arena_release(arena);
}

TEST(ReadFifoQueueMaxCount)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = device_new(arena, 0);
	u16		data[DEVICE_MAX_FIFO_VALUES];
	u8		req[] = { FC_READ_FIFO_QUEUE, 0x00, 0x05 };
	u8		resp[MODBUS_MAX_PDU];

	for (u32 i = 0; i < ARRAY_COUNT(data); i++)
		data[i] = (u16)(i + 1);
	ASSERT_EQ(device_add_fifo(dev, 5, DEVICE_MAX_FIFO_VALUES, data) == NULL);

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5 + 2 * DEVICE_MAX_FIFO_VALUES);
	ASSERT_EQ(resp[0] == FC_READ_FIFO_QUEUE);
	ASSERT_EQ(get_be16(resp + 1) == 2 + 2 * DEVICE_MAX_FIFO_VALUES);
	ASSERT_EQ(get_be16(resp + 3) == DEVICE_MAX_FIFO_VALUES);
	ASSERT_EQ(get_be16(resp + 5) == 1);
	ASSERT_EQ(get_be16(resp + 5 + 2 * (DEVICE_MAX_FIFO_VALUES - 1)) == DEVICE_MAX_FIFO_VALUES);
	arena_release(arena);
}

TEST(ReadFifoQueueZeroValues)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = device_new(arena, 0);
	u8		req[] = { FC_READ_FIFO_QUEUE, 0x00, 0x07 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(device_add_fifo(dev, 7, 0, NULL) == NULL);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(resp[0] == FC_READ_FIFO_QUEUE);
	ASSERT_EQ(get_be16(resp + 1) == 2);
	ASSERT_EQ(get_be16(resp + 3) == 0);
	arena_release(arena);
}

TEST(ReadFifoQueueRejectsMalformedRequest)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = device_new(arena, 0);
	u16		data[] = { 1 };
	u8		short_req[] = { FC_READ_FIFO_QUEUE, 0x00 };
	u8		long_req[]  = { FC_READ_FIFO_QUEUE, 0x00, 0x01, 0x00 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(device_add_fifo(dev, 1, 1, data) == NULL);

	ASSERT_EQ(run(dev, short_req, sizeof(short_req), resp) == 2);
	ASSERT_EQ(resp[0] == 0x98 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	ASSERT_EQ(run(dev, long_req, sizeof(long_req), resp) == 2);
	ASSERT_EQ(resp[0] == 0x98 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	arena_release(arena);
}

TEST(ReadFifoQueueUnconfiguredPointerIsAddressError)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = device_new(arena, 0);
	u16		data[] = { 1 };
	u8		req[] = { FC_READ_FIFO_QUEUE, 0x00, 0x02 };
	u8		resp[MODBUS_MAX_PDU];

	ASSERT_EQ(device_add_fifo(dev, 1, 1, data) == NULL);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 2);
	ASSERT_EQ(resp[0] == 0x98 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	arena_release(arena);
}

static void
assert_mei_object(const u8* resp, u32* offset, u8 id, const char* value)
{
	u32 len = (u32)strlen(value);

	ASSERT_EQ(resp[(*offset)++] == id);
	ASSERT_EQ(resp[(*offset)++] == (u8)len);
	ASSERT_EQ(memcmp(resp + *offset, value, len) == 0);
	*offset += len;
}

static void
add_extended_id(device_t* dev, u8 id, const char* value)
{
	u32 i = dev->extended_id_count++;

	dev->extended_id[i].id = id;
	strcpy(dev->extended_id[i].value, value);
}

static u32
canopen_req(u8* req, u8 control, u8 network_id, u8 node, u16 index, u8 sub,
	    u16 start, u16 count, const u8* data)
{
	u32 off = 0;

	req[off++] = FC_ENCAPSULATED_INTERFACE_TRANSPORT;
	req[off++] = MEI_TYPE_CANOPEN;
	req[off++] = control;
	req[off++] = 0;
	if (control & CANOPEN_CTRL_NETWORK_ID)
		req[off++] = network_id;
	if (control & CANOPEN_CTRL_ENCODED_DATA)
		req[off++] = 0x00; /* Unsigned8: still rejected with AE */
	req[off++] = node;
	put_be16(req + off, index);
	off += 2;
	req[off++] = sub;
	put_be16(req + off, start);
	off += 2;
	put_be16(req + off, count);
	off += 2;
	if (data)
	{
		memcpy(req + off, data, count);
		off += count;
	}
	return off;
}

static bool
canopen_ae(const u8* resp, u32 len, u8 supported)
{
	return len == 7 &&
	       resp[0] == (u8)(FC_ENCAPSULATED_INTERFACE_TRANSPORT | 0x80) &&
	       resp[1] == 0xFF && resp[2] == 0x00 && resp[3] == 0x03 &&
	       resp[4] == MEI_TYPE_CANOPEN && resp[5] == 0xAE &&
	       resp[6] == supported;
}

static bool
canopen_ce(const u8* resp, u32 len, u32 code)
{
	return len == 10 &&
	       resp[0] == (u8)(FC_ENCAPSULATED_INTERFACE_TRANSPORT | 0x80) &&
	       resp[1] == 0xFF && resp[2] == 0x00 && resp[3] == 0x06 &&
	       resp[4] == MEI_TYPE_CANOPEN && resp[5] == 0xCE &&
	       resp[6] == (u8)(code) && resp[7] == (u8)(code >> 8) &&
	       resp[8] == (u8)(code >> 16) && resp[9] == (u8)(code >> 24);
}

TEST(CanopenUpload)
{
	static const u8 obj[] = { 0x11, 0x22, 0x33, 0x44 };
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[MODBUS_MAX_PDU];
	u8		resp[MODBUS_MAX_PDU];
	u8		want[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, MEI_TYPE_CANOPEN,
			   0x00, 0x00, 0x05, 0x60, 0x41, 0x00,
			   0x00, 0x00, 0x00, 0x02, 0x11, 0x22 };
	u32		len = 0;

	ASSERT_EQ(device_add_canopen_object(dev, 5, 0x6041, 0, sizeof(obj), obj) == NULL);
	len = canopen_req(req, 0, 0, 5, 0x6041, 0, 0, 2, NULL);
	ASSERT_EQ(len == 12);
	ASSERT_EQ(run(dev, req, len, resp) == sizeof(want));
	ASSERT_EQ(memcmp(resp, want, sizeof(want)) == 0);
	arena_release(arena);
}

TEST(CanopenDownloadAndReadback)
{
	static const u8 obj[]  = { 0x11, 0x22, 0x33, 0x44 };
	static const u8 data[] = { 0xAA, 0xBB, 0xCC };
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[MODBUS_MAX_PDU];
	u8		resp[MODBUS_MAX_PDU];
	u8		want[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, MEI_TYPE_CANOPEN,
			   0x00, 0x00, 0x05, 0x60, 0x41, 0x00,
			   0x00, 0x01, 0x00, 0x03, 0xAA, 0xBB, 0xCC };
	u32		len = 0;

	ASSERT_EQ(device_add_canopen_object(dev, 5, 0x6041, 0, sizeof(obj), obj) == NULL);
	len = canopen_req(req, CANOPEN_CTRL_ACCESS, 0, 5, 0x6041, 0, 1, 3, data);
	ASSERT_EQ(len == 15);
	ASSERT_EQ(run(dev, req, len, resp) == 12);
	ASSERT_EQ(resp[0] == FC_ENCAPSULATED_INTERFACE_TRANSPORT && resp[1] == MEI_TYPE_CANOPEN);
	ASSERT_EQ(resp[2] == CANOPEN_CTRL_ACCESS && resp[3] == 0);
	ASSERT_EQ(resp[10] == 0x00 && resp[11] == 0x00); /* count echoed as zero */

	len = canopen_req(req, 0, 0, 5, 0x6041, 0, 1, 3, NULL);
	ASSERT_EQ(run(dev, req, len, resp) == sizeof(want));
	ASSERT_EQ(memcmp(resp, want, sizeof(want)) == 0);
	arena_release(arena);
}

TEST(CanopenErrors)
{
	static const u8 obj[] = { 0x11, 0x22, 0x33, 0x44 };
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[MODBUS_MAX_PDU];
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;

	ASSERT_EQ(device_add_canopen_object(dev, 5, 0x6041, 0, sizeof(obj), obj) == NULL);

	len = canopen_req(req, 0, 0, 6, 0x6041, 0, 0, 1, NULL);
	ASSERT_EQ(canopen_ce(resp, run(dev, req, len, resp), CANOPEN_ERR_OBJECT_NOT_FOUND));

	len = canopen_req(req, 0, 0, 5, 0x6041, 0, 3, 2, NULL);
	ASSERT_EQ(canopen_ce(resp, run(dev, req, len, resp), CANOPEN_ERR_LENGTH_TOO_HIGH));

	len = canopen_req(req, 0, 0, 5, 0x6041, 0, 0, 0, NULL);
	ASSERT_EQ(canopen_ce(resp, run(dev, req, len, resp), CANOPEN_ERR_INVALID_COUNT));

	len = canopen_req(req, 0, 0, 5, 0x6041, 0, 0, 0xF2, NULL);
	ASSERT_EQ(canopen_ce(resp, run(dev, req, len, resp), CANOPEN_ERR_RESPONSE_TOO_LARGE));

	len = canopen_req(req, CANOPEN_CTRL_ACCESS, 0, 5, 0x6041, 0, 0, 2, obj);
	req[len - 1] = 0; /* one data byte short */
	ASSERT_EQ(canopen_ce(resp, run(dev, req, len - 1, resp), CANOPEN_ERR_INVALID_COUNT));

	len = canopen_req(req, 0, 0, 0x7F, 0xFFFF, 0, 0, 1, NULL);
	ASSERT_EQ(canopen_ce(resp, run(dev, req, len, resp), CANOPEN_ERR_SERVICE_NOT_SUPPORTED));

	len = canopen_req(req, 0, 0, 5, 0x6041, 0, 0, 1, NULL);
	req[3] = 0x01;
	ASSERT_EQ(run(dev, req, len, resp) == 2);
	ASSERT_EQ(resp[0] == 0xAB && resp[1] == EXC_ILLEGAL_DATA_VALUE);

	ASSERT_EQ(run(dev, req, 2, resp) == 2);
	ASSERT_EQ(resp[0] == 0xAB && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	arena_release(arena);
}

TEST(CanopenUnsupportedOptions)
{
	static const u8 obj[] = { 0x11, 0x22, 0x33, 0x44 };
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[MODBUS_MAX_PDU];
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;

	ASSERT_EQ(device_add_canopen_object(dev, 5, 0x6041, 0, sizeof(obj), obj) == NULL);

	len = canopen_req(req, CANOPEN_CTRL_ENCODED_DATA, 0, 5, 0x6041, 0, 0, 2, NULL);
	ASSERT_EQ(canopen_ae(resp, run(dev, req, len, resp), 0x00));

	len = canopen_req(req, CANOPEN_CTRL_ACCESS | CANOPEN_CTRL_NETWORK_ID |
			  CANOPEN_CTRL_ENCODED_DATA, 9, 5, 0x6041, 0, 0, 2, obj);
	ASSERT_EQ(canopen_ae(resp, run(dev, req, len, resp),
			    CANOPEN_CTRL_ACCESS | CANOPEN_CTRL_NETWORK_ID));

	/* Extend, counter and a second control byte are rejected too, but the
	 * shared framing parser still accounts for their bytes. */
	len = 0;
	req[len++] = FC_ENCAPSULATED_INTERFACE_TRANSPORT;
	req[len++] = MEI_TYPE_CANOPEN;
	req[len++] = CANOPEN_CTRL_EXTEND | CANOPEN_CTRL_COUNTER | CANOPEN_CTRL_EXTENDED_CTRL;
	req[len++] = 0x00; /* second control byte */
	req[len++] = 0x00; /* reserved */
	req[len++] = 0x00; /* counter */
	req[len++] = 5;
	put_be16(req + len, 0x6041); len += 2;
	req[len++] = 0;
	put_be16(req + len, 0); len += 2;
	put_be16(req + len, 1); len += 2;
	ASSERT_EQ(canopen_ae(resp, run(dev, req, len, resp), 0x00));
	arena_release(arena);
}

TEST(CanopenNetworkIdEcho)
{
	static const u8 obj[] = { 0x11, 0x22, 0x33, 0x44 };
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[MODBUS_MAX_PDU];
	u8		resp[MODBUS_MAX_PDU];
	u8		want[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, MEI_TYPE_CANOPEN,
			   CANOPEN_CTRL_NETWORK_ID, 0x00, 0x09, 0x05, 0x60, 0x41,
			   0x00, 0x00, 0x00, 0x00, 0x02, 0x11, 0x22 };
	u32		len = 0;

	ASSERT_EQ(device_add_canopen_object(dev, 5, 0x6041, 0, sizeof(obj), obj) == NULL);
	len = canopen_req(req, CANOPEN_CTRL_NETWORK_ID, 9, 5, 0x6041, 0, 0, 2, NULL);
	ASSERT_EQ(len == 13);
	ASSERT_EQ(run(dev, req, len, resp) == sizeof(want));
	ASSERT_EQ(memcmp(resp, want, sizeof(want)) == 0);
	arena_release(arena);
}

TEST(ReadDeviceIdentificationBasicStream)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x01, 0x00 };
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	u32		off = 7;

	len = run(dev, req, sizeof(req), resp);
	ASSERT_EQ(resp[0] == FC_ENCAPSULATED_INTERFACE_TRANSPORT);
	ASSERT_EQ(resp[1] == 0x0E);
	ASSERT_EQ(resp[2] == 0x01);
	ASSERT_EQ(resp[3] == 0x81); /* basic only: no regular objects configured */
	ASSERT_EQ(resp[4] == 0x00);
	ASSERT_EQ(resp[5] == 0x00);
	ASSERT_EQ(resp[6] == 0x03);
	assert_mei_object(resp, &off, 0x00, "modbus-sim");
	assert_mei_object(resp, &off, 0x01, "modbus-sim");
	assert_mei_object(resp, &off, 0x02, "1.0");
	ASSERT_EQ(len == off);
	arena_release(arena);
}

TEST(ReadDeviceIdentificationRegularAndExtended)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x02, 0x00 };
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;
	u32		off = 7;

	strcpy(dev->device_id[DEVICE_ID_VENDOR_URL], "https://example.invalid");
	strcpy(dev->device_id[DEVICE_ID_PRODUCT_NAME], "Simulator");
	strcpy(dev->device_id[DEVICE_ID_MODEL_NAME], "M1");
	strcpy(dev->device_id[DEVICE_ID_USER_APPLICATION_NAME], "TestApp");

	len = run(dev, req, sizeof(req), resp);
	ASSERT_EQ(resp[2] == 0x02);
	ASSERT_EQ(resp[3] == 0x82);
	ASSERT_EQ(resp[4] == 0x00);
	ASSERT_EQ(resp[6] == 0x07);
	assert_mei_object(resp, &off, 0x00, "modbus-sim");
	assert_mei_object(resp, &off, 0x01, "modbus-sim");
	assert_mei_object(resp, &off, 0x02, "1.0");
	assert_mei_object(resp, &off, 0x03, "https://example.invalid");
	assert_mei_object(resp, &off, 0x04, "Simulator");
	assert_mei_object(resp, &off, 0x05, "M1");
	assert_mei_object(resp, &off, 0x06, "TestApp");
	ASSERT_EQ(len == off);

	req[2] = 0x03;
	req[3] = 0x03;
	len = run(dev, req, sizeof(req), resp);
	off = 7;
	ASSERT_EQ(resp[2] == 0x03);
	ASSERT_EQ(resp[3] == 0x82);
	ASSERT_EQ(resp[4] == 0x00);
	ASSERT_EQ(resp[6] == 0x04);
	assert_mei_object(resp, &off, 0x03, "https://example.invalid");
	assert_mei_object(resp, &off, 0x04, "Simulator");
	assert_mei_object(resp, &off, 0x05, "M1");
	assert_mei_object(resp, &off, 0x06, "TestApp");
	ASSERT_EQ(len == off);
	arena_release(arena);
}

TEST(ReadDeviceIdentificationSpecific)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x04, 0x01 };
	u8		resp[MODBUS_MAX_PDU];
	u32		off = 7;

	strcpy(dev->device_id[DEVICE_ID_PRODUCT_NAME], "Simulator");
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 7 + 2 + 10);
	ASSERT_EQ(resp[2] == 0x04);
	ASSERT_EQ(resp[3] == 0x82);
	ASSERT_EQ(resp[4] == 0x00);
	ASSERT_EQ(resp[5] == 0x00);
	ASSERT_EQ(resp[6] == 0x01);
	assert_mei_object(resp, &off, 0x01, "modbus-sim");

	req[3] = 0x04;
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 7 + 2 + 9);
	off = 7;
	assert_mei_object(resp, &off, 0x04, "Simulator");
	arena_release(arena);
}

TEST(ReadDeviceIdentificationSpecificUnknownObjectIsAddressError)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[]  = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x04, 0x03 };
	u8		resp[MODBUS_MAX_PDU];

	/* 0x03 is a known regular id but not configured on this basic-only device. */
	run(dev, req, sizeof(req), resp);
	ASSERT_EQ(resp[0] == 0xAB && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	arena_release(arena);
}

TEST(ReadDeviceIdentificationExtendedObjects)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		stream[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x03, 0x00 };
	u8		resume[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x03, 0x90 };
	u8		basic[]  = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x01, 0x00 };
	u8		regular[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x02, 0x00 };
	u8		specific[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x04, 0x90 };
	u8		unknown[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x03, 0x7F };
	u8		absent[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x04, 0x81 };
	u8		resp[MODBUS_MAX_PDU];
	u32		off = 7;

	strcpy(dev->device_id[DEVICE_ID_VENDOR_URL], "url");
	strcpy(dev->device_id[DEVICE_ID_PRODUCT_NAME], "Sim");
	add_extended_id(dev, 0xA0, "Zulu");
	add_extended_id(dev, 0x80, "Alpha");
	add_extended_id(dev, 0x90, "Mike");

	run(dev, stream, sizeof(stream), resp);
	ASSERT_EQ(resp[3] == 0x83); /* extended conformity takes precedence */
	ASSERT_EQ(resp[4] == 0x00 && resp[5] == 0x00 && resp[6] == 0x08);
	assert_mei_object(resp, &off, 0x00, "modbus-sim");
	assert_mei_object(resp, &off, 0x01, "modbus-sim");
	assert_mei_object(resp, &off, 0x02, "1.0");
	assert_mei_object(resp, &off, 0x03, "url");
	assert_mei_object(resp, &off, 0x04, "Sim");
	assert_mei_object(resp, &off, 0x80, "Alpha");
	assert_mei_object(resp, &off, 0x90, "Mike");
	assert_mei_object(resp, &off, 0xA0, "Zulu");

	off = 7;
	run(dev, resume, sizeof(resume), resp);
	ASSERT_EQ(resp[3] == 0x83 && resp[6] == 0x02);
	assert_mei_object(resp, &off, 0x90, "Mike");
	assert_mei_object(resp, &off, 0xA0, "Zulu");

	off = 7;
	run(dev, unknown, sizeof(unknown), resp);
	ASSERT_EQ(resp[6] == 0x08 && resp[7] == 0x00);
	assert_mei_object(resp, &off, 0x00, "modbus-sim");
	assert_mei_object(resp, &off, 0x01, "modbus-sim");
	assert_mei_object(resp, &off, 0x02, "1.0");
	assert_mei_object(resp, &off, 0x03, "url");
	assert_mei_object(resp, &off, 0x04, "Sim");
	assert_mei_object(resp, &off, 0x80, "Alpha");
	assert_mei_object(resp, &off, 0x90, "Mike");
	assert_mei_object(resp, &off, 0xA0, "Zulu");

	off = 7;
	run(dev, basic, sizeof(basic), resp);
	ASSERT_EQ(resp[3] == 0x83 && resp[4] == 0x00 && resp[6] == 0x03);
	assert_mei_object(resp, &off, 0x00, "modbus-sim");
	assert_mei_object(resp, &off, 0x01, "modbus-sim");
	assert_mei_object(resp, &off, 0x02, "1.0");

	off = 7;
	run(dev, regular, sizeof(regular), resp);
	ASSERT_EQ(resp[3] == 0x83 && resp[4] == 0x00 && resp[6] == 0x05);
	assert_mei_object(resp, &off, 0x00, "modbus-sim");
	assert_mei_object(resp, &off, 0x01, "modbus-sim");
	assert_mei_object(resp, &off, 0x02, "1.0");
	assert_mei_object(resp, &off, 0x03, "url");
	assert_mei_object(resp, &off, 0x04, "Sim");

	off = 7;
	run(dev, specific, sizeof(specific), resp);
	ASSERT_EQ(resp[3] == 0x83 && resp[4] == 0x00 && resp[5] == 0x00 && resp[6] == 0x01);
	assert_mei_object(resp, &off, 0x90, "Mike");

	run(dev, absent, sizeof(absent), resp);
	ASSERT_EQ(resp[0] == 0xAB && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	arena_release(arena);
}

TEST(ReadDeviceIdentificationExtendedMoreFollows)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		req[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x03, 0x00 };
	u8		resp[MODBUS_MAX_PDU];
	char		value[131];
	u32		len = 0;
	u32		off = 7;

	memset(value, 'E', 130);
	value[130] = '\0';
	add_extended_id(dev, 0x80, value);
	add_extended_id(dev, 0x90, value);
	add_extended_id(dev, 0xA0, value);

	len = run(dev, req, sizeof(req), resp);
	ASSERT_EQ(len <= MODBUS_MAX_PDU);
	ASSERT_EQ(resp[3] == 0x83 && resp[4] == 0xFF && resp[5] == 0x90 && resp[6] == 0x04);
	assert_mei_object(resp, &off, 0x00, "modbus-sim");
	assert_mei_object(resp, &off, 0x01, "modbus-sim");
	assert_mei_object(resp, &off, 0x02, "1.0");
	assert_mei_object(resp, &off, 0x80, value);
	ASSERT_EQ(len == off);

	req[3] = resp[5];
	len = run(dev, req, sizeof(req), resp);
	ASSERT_EQ(len <= MODBUS_MAX_PDU);
	ASSERT_EQ(resp[4] == 0xFF && resp[5] == 0xA0 && resp[6] == 0x01 && resp[7] == 0x90);
	ASSERT_EQ(len == 7 + 2 + 130);

	req[3] = resp[5];
	len = run(dev, req, sizeof(req), resp);
	ASSERT_EQ(len <= MODBUS_MAX_PDU);
	ASSERT_EQ(resp[4] == 0x00 && resp[5] == 0x00 && resp[6] == 0x01 && resp[7] == 0xA0);
	ASSERT_EQ(len == 7 + 2 + 130);
	arena_release(arena);
}

TEST(ReadDeviceIdentificationMoreFollows)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = device_new(arena, 0);
	u8		req[]  = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x01, 0x00 };
	u8		resp[MODBUS_MAX_PDU];
	u32		len = 0;

	/* 7-byte MEI header + 2-byte object header + 244-byte value = 253 (MODBUS_MAX_PDU). */
	memset(dev->device_id[DEVICE_ID_VENDOR_NAME], 'V', DEVICE_ID_OBJECT_MAX);
	dev->device_id[DEVICE_ID_VENDOR_NAME][DEVICE_ID_OBJECT_MAX] = '\0';
	memset(dev->device_id[DEVICE_ID_PRODUCT_CODE], 'P', DEVICE_ID_OBJECT_MAX);
	dev->device_id[DEVICE_ID_PRODUCT_CODE][DEVICE_ID_OBJECT_MAX] = '\0';
	memset(dev->device_id[DEVICE_ID_MAJOR_MINOR_REVISION], 'R', DEVICE_ID_OBJECT_MAX);
	dev->device_id[DEVICE_ID_MAJOR_MINOR_REVISION][DEVICE_ID_OBJECT_MAX] = '\0';

	len = run(dev, req, sizeof(req), resp);
	ASSERT_EQ(resp[0] == FC_ENCAPSULATED_INTERFACE_TRANSPORT);
	ASSERT_EQ(resp[4] == 0xFF);
	ASSERT_EQ(resp[5] == 0x01);
	ASSERT_EQ(resp[6] == 0x01);
	ASSERT_EQ(resp[7] == 0x00);
	ASSERT_EQ(resp[8] == DEVICE_ID_OBJECT_MAX);
	ASSERT_EQ(len == 7 + 2 + DEVICE_ID_OBJECT_MAX);

	req[3] = resp[5];
	len = run(dev, req, sizeof(req), resp);
	ASSERT_EQ(resp[4] == 0xFF);
	ASSERT_EQ(resp[5] == 0x02);
	ASSERT_EQ(resp[6] == 0x01);
	ASSERT_EQ(resp[7] == 0x01);
	ASSERT_EQ(resp[8] == DEVICE_ID_OBJECT_MAX);
	ASSERT_EQ(len == 7 + 2 + DEVICE_ID_OBJECT_MAX);

	req[3] = resp[5];
	len = run(dev, req, sizeof(req), resp);
	ASSERT_EQ(resp[4] == 0x00);
	ASSERT_EQ(resp[5] == 0x00);
	ASSERT_EQ(resp[6] == 0x01);
	ASSERT_EQ(resp[7] == 0x02);
	ASSERT_EQ(resp[8] == DEVICE_ID_OBJECT_MAX);
	ASSERT_EQ(len == 7 + 2 + DEVICE_ID_OBJECT_MAX);
	arena_release(arena);
}

TEST(ReadDeviceIdentificationErrors)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		bad_mei[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0F, 0x01, 0x00 };
	u8		bad_mei_short[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0F, 0x01 };
	u8		mei13_bad_len[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D, 0x01 };
	u8		mei13_bad_code[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0D, 0x05, 0x00 };
	u8		bad_code[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x05, 0x00 };
	u8		bad_len[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x01 };
	u8		bad_spec[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x04, 0x04 };
	u8		bad_spec_range[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x04, 0x07 };
	u8		bad_obj[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x04, 0x80 };
	u8		resp[MODBUS_MAX_PDU];

	run(dev, bad_mei, sizeof(bad_mei), resp);
	ASSERT_EQ(resp[0] == 0xAB && resp[1] == EXC_ILLEGAL_FUNCTION);
	/* MEI type is checked before request length so unsupported MEI always gets 01. */
	run(dev, bad_mei_short, sizeof(bad_mei_short), resp);
	ASSERT_EQ(resp[0] == 0xAB && resp[1] == EXC_ILLEGAL_FUNCTION);
	/* MEI 0x0D is CANopen: a truncated header is an illegal data value. */
	run(dev, mei13_bad_len, sizeof(mei13_bad_len), resp);
	ASSERT_EQ(resp[0] == 0xAB && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, mei13_bad_code, sizeof(mei13_bad_code), resp);
	ASSERT_EQ(resp[0] == 0xAB && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_code, sizeof(bad_code), resp);
	ASSERT_EQ(resp[0] == 0xAB && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_len, sizeof(bad_len), resp);
	ASSERT_EQ(resp[0] == 0xAB && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	run(dev, bad_spec, sizeof(bad_spec), resp);
	ASSERT_EQ(resp[0] == 0xAB && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	run(dev, bad_spec_range, sizeof(bad_spec_range), resp);
	ASSERT_EQ(resp[0] == 0xAB && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	run(dev, bad_obj, sizeof(bad_obj), resp);
	ASSERT_EQ(resp[0] == 0xAB && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	arena_release(arena);
}

TEST(ReadDeviceIdentificationStreamRestartsAtObject0)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		basic[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x01, 0x03 };
	u8		regular[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x02, 0x07 };
	u8		extended[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, 0x03, 0xFF };
	u8		resp[MODBUS_MAX_PDU];
	u32		off = 7;

	strcpy(dev->device_id[DEVICE_ID_PRODUCT_NAME], "Simulator");

	run(dev, basic, sizeof(basic), resp);
	ASSERT_EQ(resp[0] == FC_ENCAPSULATED_INTERFACE_TRANSPORT);
	ASSERT_EQ(resp[2] == 0x01);
	ASSERT_EQ(resp[3] == 0x82);
	ASSERT_EQ(resp[4] == 0x00);
	ASSERT_EQ(resp[6] == 0x03);
	assert_mei_object(resp, &off, 0x00, "modbus-sim");
	assert_mei_object(resp, &off, 0x01, "modbus-sim");
	assert_mei_object(resp, &off, 0x02, "1.0");

	off = 7;
	run(dev, regular, sizeof(regular), resp);
	ASSERT_EQ(resp[2] == 0x02);
	ASSERT_EQ(resp[3] == 0x82);
	ASSERT_EQ(resp[4] == 0x00);
	ASSERT_EQ(resp[6] == 0x04);
	assert_mei_object(resp, &off, 0x00, "modbus-sim");
	assert_mei_object(resp, &off, 0x01, "modbus-sim");
	assert_mei_object(resp, &off, 0x02, "1.0");
	assert_mei_object(resp, &off, 0x04, "Simulator");

	off = 7;
	run(dev, extended, sizeof(extended), resp);
	ASSERT_EQ(resp[2] == 0x03);
	ASSERT_EQ(resp[3] == 0x82);
	ASSERT_EQ(resp[4] == 0x00);
	ASSERT_EQ(resp[6] == 0x04);
	assert_mei_object(resp, &off, 0x00, "modbus-sim");
	assert_mei_object(resp, &off, 0x01, "modbus-sim");
	assert_mei_object(resp, &off, 0x02, "1.0");
	assert_mei_object(resp, &off, 0x04, "Simulator");
	arena_release(arena);
}

TEST(ExceptionFaultReplacesResponseAfterProcessing)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			write[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0xAB, 0xCD };
	u8			resp[MODBUS_MAX_PDU];
	u16			value = 0;

	set_fault(dev, FAULT_EXCEPTION, 0, EXC_SLAVE_DEVICE_BUSY, 100);
	rng_seed(&rng, 1);
	ASSERT_EQ(modbus_handle_request(dev, &rng, 0, write, sizeof(write), resp, &decision) == 2);
	ASSERT_EQ(resp[0] == (FC_WRITE_SINGLE_REGISTER | 0x80) && resp[1] == EXC_SLAVE_DEVICE_BUSY);
	ASSERT_EQ(decision.exception_code == EXC_SLAVE_DEVICE_BUSY && !decision.corrupt);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 1, &value));
	ASSERT_EQ(value == 0xABCD);
	ASSERT_EQ(dev->message_count == 1 && dev->com_event_count == 1);

	ASSERT_EQ(modbus_handle_request(dev, &rng, 0, NULL, 0, resp, &decision) == 2);
	ASSERT_EQ(resp[0] == 0x80 && resp[1] == EXC_SLAVE_DEVICE_BUSY);
	arena_release(arena);
}

TEST(CorruptFaultFlipsPayloadAfterProcessing)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			read[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x02 };
	u8			write[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0x11, 0x22 };
	u8			resp[MODBUS_MAX_PDU];
	u16			value = 0;

	set_fault(dev, FAULT_CORRUPT, 0, 0, 100);
	rng_seed(&rng, 2);
	ASSERT_EQ(modbus_handle_request(dev, &rng, 0, read, sizeof(read), resp, &decision) == 6);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS && resp[1] == (0x04 ^ 0xFF));
	ASSERT_EQ(resp[2] == 0x12 && resp[3] == 0x34 && resp[4] == 0x56 && resp[5] == 0x78);
	ASSERT_EQ(decision.corrupt && decision.exception_code == 0);

	rng_seed(&rng, 3);
	ASSERT_EQ(modbus_handle_request(dev, &rng, 0, write, sizeof(write), resp, &decision) == 5);
	ASSERT_EQ(resp[0] == FC_WRITE_SINGLE_REGISTER && resp[1] == (0x00 ^ 0xFF));
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 1, &value));
	ASSERT_EQ(value == 0x1122);
	arena_release(arena);
}

TEST(FaultsAtZeroPercentLeaveResponsesUntouched)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8			resp[MODBUS_MAX_PDU];
	u8			want[] = { FC_READ_HOLDING_REGISTERS, 0x02, 0x12, 0x34 };

	rng_seed(&rng, 4);
	set_fault(dev, FAULT_EXCEPTION, 0, EXC_MEMORY_PARITY_ERROR, 0);
	ASSERT_EQ(modbus_handle_request(dev, &rng, 0, req, sizeof(req), resp, &decision) == sizeof(want));
	ASSERT_EQ(memcmp(resp, want, sizeof(want)) == 0);

	dev->faults.present = 0;
	set_fault(dev, FAULT_CORRUPT, 0, 0, 0);
	ASSERT_EQ(modbus_handle_request(dev, &rng, 0, req, sizeof(req), resp, &decision) == sizeof(want));
	ASSERT_EQ(memcmp(resp, want, sizeof(want)) == 0);
	arena_release(arena);
}

TEST(MultipleFaultsCombineInOneResponse)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			req[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8			resp[MODBUS_MAX_PDU];

	set_fault(dev, FAULT_DELAY, 150, 0, 100);
	set_fault(dev, FAULT_EXCEPTION, 0, EXC_SLAVE_DEVICE_BUSY, 100);
	set_fault(dev, FAULT_CORRUPT, 0, 0, 100);
	rng_seed(&rng, 21);
	ASSERT_EQ(modbus_handle_request(dev, &rng, 0, req, sizeof(req), resp, &decision) == 2);
	ASSERT_EQ(resp[0] == (FC_READ_HOLDING_REGISTERS | 0x80));
	ASSERT_EQ(resp[1] == (EXC_SLAVE_DEVICE_BUSY ^ 0xFF)); /* corrupt applies to the exception */
	ASSERT_EQ(decision.delay_ms == 150 && decision.exception_code == EXC_SLAVE_DEVICE_BUSY);
	ASSERT_EQ(decision.corrupt && !decision.drop && !decision.disconnect);

	dev->faults.present = 0;
	set_fault(dev, FAULT_DROP, 0, 0, 100);
	set_fault(dev, FAULT_EXCEPTION, 0, EXC_MEMORY_PARITY_ERROR, 100);
	rng_seed(&rng, 22);
	ASSERT_EQ(modbus_handle_request(dev, &rng, 0, req, sizeof(req), resp, &decision) == 2);
	ASSERT_EQ(decision.drop && decision.exception_code == EXC_MEMORY_PARITY_ERROR);
	ASSERT_EQ(!decision.disconnect);
	arena_release(arena);
}

TEST(SilentRequestSuppressesDisconnectFault)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			resp[MODBUS_MAX_PDU];
	u8			listen[] = { FC_DIAGNOSTICS, 0x00, 0x04, 0x00, 0x00 };
	u8			read[]   = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };

	set_fault(dev, FAULT_DISCONNECT, 0, 0, 100);
	rng_seed(&rng, 23);
	ASSERT_EQ(modbus_handle_request(dev, &rng, 0, listen, sizeof(listen), resp, &decision) == 0);
	ASSERT_EQ(dev->listen_only && decision.drop && !decision.disconnect);

	rng_seed(&rng, 24);
	ASSERT_EQ(modbus_handle_request(dev, &rng, 0, read, sizeof(read), resp, &decision) == 0);
	ASSERT_EQ(decision.drop && !decision.disconnect);
	arena_release(arena);
}
