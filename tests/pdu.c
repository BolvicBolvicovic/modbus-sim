#include "vtest.h"
#include "device/machine.h"
#include "modbus/pdu.h"
#include "modbus/mbap.h"
#include "modbus/bytes.h"
#include "modbus/mei_canopen.h"
#include "vlib/macros.h"
#include <stdio.h>
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

static void
set_corrupt_fault(device_t* dev, fault_corrupt_mode_t mode, u32 percent)
{
	dev->faults.by_kind[FAULT_CORRUPT] = (fault_t){
		.corrupt_mode = mode,
		.percent      = percent,
	};
	dev->faults.present |= FAULT_BIT(FAULT_CORRUPT);
}

static void
set_scheduled_fault(device_t* dev, fault_kind_t kind,
		    const fault_schedule_step_t* steps, u8 step_count, u64 schedule_id)
{
	fault_t* fault = &dev->faults.by_kind[kind];

	*fault = (fault_t){
		.schedule_count = step_count,
		.schedule_id    = schedule_id,
	};
	for (u8 i = 0; i < step_count; i++)
		fault->schedule[i] = steps[i];
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

static u32
run_request_at(device_t* dev, rng_t* rng, u64 now_ms, const u8* req, u32 len, u8* resp,
	       fault_decision_t* decision)
{
	return modbus_handle_request(dev, rng, now_ms, req, len, resp, decision);
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

static void
assert_exception_counters(device_t* dev, u16 exceptions, u16 busy, u16 nak)
{
	u8	req[5];
	u8	resp[MODBUS_MAX_PDU];

	diagnostic_request(req, DIAG_SUB_RETURN_BUS_EXCEPTION_ERROR_COUNT, 0);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(get_be16(resp + 3) == exceptions);
	diagnostic_request(req, DIAG_SUB_RETURN_SLAVE_BUSY_COUNT, 0);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(get_be16(resp + 3) == busy);
	diagnostic_request(req, DIAG_SUB_RETURN_SLAVE_NAK_COUNT, 0);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(get_be16(resp + 3) == nak);
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

typedef enum {
	PDU_DEV_BASE,
	PDU_DEV_FILES,
	PDU_DEV_FIFOS,
	PDU_DEV_HOLD10,
	PDU_DEV_HOLD1,
} pdu_dev_t;

typedef struct pdu_case_t pdu_case_t;

/* A named scenario: a device (plus presets), then requests run in order, each
 * answered exactly as listed. A step with req_len 0 ends the list. */
struct pdu_case_t {
	const char*	name;
	pdu_dev_t	dev;
	bool		via_request;	/* modbus_handle_request instead of modbus_handle_pdu */
	struct pdu_preset {
		bool	set_id;
		u8	server_id;
		u8	run_indicator;
		u8	exception_status;
		u8	extra[4];
		u8	extra_len;
		u16	com_status;
		u16	com_event_count;
		u16	message_count;
		u8	events[3];
		u8	event_log_count;
	} preset;
	struct pdu_step {
		u8	req[40];
		u32	req_len;
		u8	resp[72];
		u32	resp_len;
	} steps[6];
};

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

static device_t*
make_hold_device(arena_t* arena, f64 first)
{
	device_t*	dev = device_new(arena, 0);
	point_t		p   = { 0 };

	p.kind  = POINT_STATIC;
	p.space = SPACE_HOLDING;
	p.type  = VT_U16;
	for (u32 i = 0; i < 4; i++)
	{
		p.address      = i;
		p.static_value = first + (f64)i;
		device_add_point(dev, &p);
	}
	return dev;
}

static device_t*
make_fifo_device(arena_t* arena)
{
	device_t*	dev    = device_new(arena, 0);
	u16		data[] = { 0x1234, 0x5678, 0x0003 };
	u16		pair[] = { 0x0001, 0x0002 };

	device_add_fifo(dev, 10, 3, data);
	device_add_fifo(dev, 1, 0, pair);
	device_add_fifo(dev, 2, 2, pair);
	device_add_fifo(dev, 7, 0, NULL);
	return dev;
}

static void
run_pdu_cases(const pdu_case_t* cases, u32 count)
{
	for (u32 i = 0; i < count; i++)
	{
		const pdu_case_t*	c     = &cases[i];
		arena_t*		arena = ARENA_ALLOC();
		device_t*		dev   = NULL;
		rng_t			rng;
		fault_decision_t	decision = { 0 };
		u8			resp[MODBUS_MAX_PDU];

		switch (c->dev)
		{
		case PDU_DEV_BASE:	dev = make_device(arena); break;
		case PDU_DEV_FILES:	dev = make_file_device(arena); break;
		case PDU_DEV_FIFOS:	dev = make_fifo_device(arena); break;
		case PDU_DEV_HOLD10:	dev = make_hold_device(arena, 10); break;
		case PDU_DEV_HOLD1:	dev = make_hold_device(arena, 1); break;
		}
		rng_seed(&rng, 1);
		if (c->preset.set_id)
		{
			dev->server_id     = c->preset.server_id;
			dev->run_indicator = c->preset.run_indicator;
		}
		dev->exception_status = c->preset.exception_status;
		dev->server_id_additional_data_len = c->preset.extra_len;
		memcpy(dev->server_id_additional_data, c->preset.extra, c->preset.extra_len);
		if (c->preset.com_status != 0)
			dev->com_status = c->preset.com_status;
		dev->com_event_count = c->preset.com_event_count;
		dev->message_count   = c->preset.message_count;
		memcpy(dev->com_events, c->preset.events, sizeof(c->preset.events));
		dev->com_event_log_count = c->preset.event_log_count;

		for (u32 j = 0; j < ARRAY_COUNT(c->steps) && c->steps[j].req_len != 0; j++)
		{
			const struct pdu_step*	s   = &c->steps[j];
			u32			len = c->via_request ?
				run_request(dev, &rng, s->req, s->req_len, resp, &decision) :
				run(dev, s->req, s->req_len, resp);
			bool			ok  = len == s->resp_len &&
						      memcmp(resp, s->resp, len) == 0;

			if (!ok)
				fprintf(stderr, "pdu case '%s' step %u failed\n", c->name, j);
			ASSERT_EQ(ok);
		}
		arena_release(arena);
	}
}

#define READ1   { 0x03, 0x00, 0x00, 0x00, 0x01 }, 5, { 0x03, 0x02, 0x12, 0x34 }, 4
#define BAD_QTY { 0x03, 0x00, 0x00, 0x00, 0x00 }, 5, { 0x83, 0x03 }, 2

TEST(MalformedStatusAndEventRequestsAreIllegalDataValue)
{
	static const pdu_case_t cases[] = {
		{ "fc07 extra byte", PDU_DEV_BASE, .steps = { { { 0x07, 0x00 }, 2, { 0x87, 0x03 }, 2 } } },
		{ "fc11 extra byte", PDU_DEV_BASE, .steps = { { { 0x11, 0x00 }, 2, { 0x91, 0x03 }, 2 } } },
		{ "fc0b extra byte", PDU_DEV_BASE, .steps = { { { 0x0B, 0x00 }, 2, { 0x8B, 0x03 }, 2 } } },
		{ "fc0c extra byte", PDU_DEV_BASE, .steps = { { { 0x0C, 0x00 }, 2, { 0x8C, 0x03 }, 2 } } },
		{ "fc0b/fc0c extra byte keep configured values", PDU_DEV_BASE,
		  .preset = { .com_status = 0xFFFF, .events = { 0x42 }, .event_log_count = 1 },
		  .steps = { { { 0x0B, 0x00 }, 2, { 0x8B, 0x03 }, 2 },
			     { { 0x0C, 0x00 }, 2, { 0x8C, 0x03 }, 2 },
			     /* exceptions are not events; both malformed PDUs counted as messages */
			     { { 0x0C }, 1, { 0x0C, 0x07, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x03, 0x42 }, 9 } } },
		{ "fc16 short", PDU_DEV_BASE,
		  .steps = { { { 0x16, 0x00, 0x00, 0xFF, 0xFF, 0x00 }, 6, { 0x96, 0x03 }, 2 } } },
		{ "fc16 long", PDU_DEV_BASE,
		  .steps = { { { 0x16, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00 }, 8, { 0x96, 0x03 }, 2 } } },
		{ "fc18 short", PDU_DEV_FIFOS, .steps = { { { 0x18, 0x00 }, 2, { 0x98, 0x03 }, 2 } } },
		{ "fc18 long", PDU_DEV_FIFOS, .steps = { { { 0x18, 0x00, 0x01, 0x00 }, 4, { 0x98, 0x03 }, 2 } } },
	};

	run_pdu_cases(cases, ARRAY_COUNT(cases));
}

TEST(ReadExceptionStatusAndReportServerId)
{
	static const pdu_case_t cases[] = {
		{ "fc07 default is zero", PDU_DEV_BASE, .steps = { { { 0x07 }, 1, { 0x07, 0x00 }, 2 } } },
		{ "fc07 configured byte", PDU_DEV_BASE, .preset = { .exception_status = 0x5A },
		  .steps = { { { 0x07 }, 1, { 0x07, 0x5A }, 2 } } },
		{ "fc11 defaults", PDU_DEV_BASE, .steps = { { { 0x11 }, 1, { 0x11, 0x02, 0x00, 0xFF }, 4 } } },
		{ "fc11 configured values", PDU_DEV_BASE, .preset = { .set_id = true, .server_id = 0x2A },
		  .steps = { { { 0x11 }, 1, { 0x11, 0x02, 0x2A, 0x00 }, 4 } } },
		{ "fc11 additional data", PDU_DEV_BASE,
		  .preset = { .set_id = true, .server_id = 0x2A, .extra = { 0xDE, 0xAD, 0xBE, 0xEF }, .extra_len = 4 },
		  .steps = { { { 0x11 }, 1, { 0x11, 0x06, 0x2A, 0x00, 0xDE, 0xAD, 0xBE, 0xEF }, 8 } } },
	};

	run_pdu_cases(cases, ARRAY_COUNT(cases));
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

TEST(GetComEventCounterAndLog)
{
	static const pdu_case_t cases[] = {
		{ "fc0b status and count", PDU_DEV_BASE,
		  .steps = { { READ1 }, { { 0x0B }, 1, { 0x0B, 0, 0, 0, 1 }, 5 } } },
		{ "fc0b counts successful completions only", PDU_DEV_BASE,
		  .steps = { { { 0x0B }, 1, { 0x0B, 0, 0, 0, 0 }, 5 }, { READ1 }, { BAD_QTY },
			     { { 0x0B }, 1, { 0x0B, 0, 0, 0, 1 }, 5 } } },
		{ "fc0b wraps at 16 bits", PDU_DEV_BASE, .preset = { .com_event_count = 0xFFFF },
		  .steps = { { READ1 }, { { 0x0B }, 1, { 0x0B, 0, 0, 0, 0 }, 5 } } },
		{ "fc0b configured status is not an event", PDU_DEV_BASE, .preset = { .com_status = 0xFFFF },
		  .steps = { { { 0x0B }, 1, { 0x0B, 0xFF, 0xFF, 0, 0 }, 5 } } },
		{ "fc0c counts and empty log", PDU_DEV_BASE,
		  .steps = { { READ1 }, { { 0x0C }, 1, { 0x0C, 6, 0, 0, 0, 1, 0, 2 }, 8 } } },
		{ "fc0c matches fc0b", PDU_DEV_BASE,
		  .steps = { { READ1 }, { { 0x0B }, 1, { 0x0B, 0, 0, 0, 1 }, 5 }, { BAD_QTY },
			     { { 0x0C }, 1, { 0x0C, 6, 0, 0, 0, 1, 0, 4 }, 8 },
			     { { 0x0B }, 1, { 0x0B, 0, 0, 0, 1 }, 5 } } },
		{ "fc0c wraps at 16 bits", PDU_DEV_BASE,
		  .preset = { .com_event_count = 0xFFFF, .message_count = 0xFFFF },
		  .steps = { { { 0x0C }, 1, { 0x0C, 6, 0, 0, 0xFF, 0xFF, 0, 0 }, 8 } } },
		{ "fc0c configured status and events", PDU_DEV_BASE,
		  .preset = { .com_status = 0xFFFF, .events = { 0x01, 0x80, 0xFF }, .event_log_count = 3 },
		  .steps = { { { 0x0C }, 1, { 0x0C, 9, 0xFF, 0xFF, 0, 0, 0, 1, 0x01, 0x80, 0xFF }, 11 } } },
		/* modbus_handle_request: FC 0B/0C are never counted as events themselves */
		{ "fc0c is not an event on the request path", PDU_DEV_BASE, true,
		  .steps = { { READ1 }, { { 0x0C }, 1, { 0x0C, 6, 0, 0, 0, 1, 0, 2 }, 8 },
			     { { 0x0C }, 1, { 0x0C, 6, 0, 0, 0, 1, 0, 3 }, 8 },
			     { { 0x0B }, 1, { 0x0B, 0, 0, 0, 1 }, 5 } } },
	};

	run_pdu_cases(cases, ARRAY_COUNT(cases));
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

#define BYTES(...) { __VA_ARGS__ }, sizeof((const u8[]){ __VA_ARGS__ })
#define EXC(fc, code) BYTES(fc), BYTES((fc) | 0x80, code)

TEST(FileRecordRequests)
{
	/* file 1: record 0-1, 10-12; file 4: record 2 */
	static const pdu_case_t cases[] = {
		{ "read single sub-request", PDU_DEV_FILES,
		  .steps = { { BYTES(0x14, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02),
			       BYTES(0x14, 0x06, 0x05, 0x06, 0x12, 0x34, 0x56, 0x78) } } },
		{ "read multiple sub-requests", PDU_DEV_FILES,
		  .steps = { { BYTES(0x14, 0x0E, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
				     0x06, 0x00, 0x04, 0x00, 0x02, 0x00, 0x01),
			       BYTES(0x14, 0x08, 0x03, 0x06, 0x12, 0x34, 0x03, 0x06, 0x00, 0xFF) } } },
		{ "write single sub-request echoes and updates", PDU_DEV_FILES,
		  .steps = { { BYTES(0x15, 0x0B, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0xAA, 0xAA, 0xBB, 0xBB),
			       BYTES(0x15, 0x0B, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0xAA, 0xAA, 0xBB, 0xBB) },
			     { BYTES(0x14, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02),
			       BYTES(0x14, 0x06, 0x05, 0x06, 0xAA, 0xAA, 0xBB, 0xBB) } } },
		{ "write multiple sub-requests", PDU_DEV_FILES,
		  .steps = { { BYTES(0x15, 0x12, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x11,
				     0x06, 0x00, 0x04, 0x00, 0x02, 0x00, 0x01, 0x00, 0x22),
			       BYTES(0x15, 0x12, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x11,
				     0x06, 0x00, 0x04, 0x00, 0x02, 0x00, 0x01, 0x00, 0x22) },
			     { BYTES(0x14, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02),
			       BYTES(0x14, 0x06, 0x05, 0x06, 0x00, 0x11, 0x56, 0x78) },
			     { BYTES(0x14, 0x07, 0x06, 0x00, 0x04, 0x00, 0x02, 0x00, 0x01),
			       BYTES(0x14, 0x04, 0x03, 0x06, 0x00, 0x22) } } },
		{ "write is all or nothing", PDU_DEV_FILES,
		  .steps = { { BYTES(0x15, 0x12, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x77,
				     0x06, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01, 0x00, 0x88),
			       BYTES(0x95, 0x02) },
			     { BYTES(0x14, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01),
			       BYTES(0x14, 0x04, 0x03, 0x06, 0x12, 0x34) } } },
		/* FC 20 malformed */
		{ "read: missing sub-request", PDU_DEV_FILES, .steps = { { EXC(0x14, 0x03) } } },
		{ "read: byte count mismatch", PDU_DEV_FILES,
		  .steps = { { BYTES(0x14, 0x06, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01), BYTES(0x94, 0x03) } } },
		{ "read: byte count not multiple of 7", PDU_DEV_FILES,
		  .steps = { { BYTES(0x14, 0x08, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00), BYTES(0x94, 0x03) } } },
		{ "read: byte count above maximum", PDU_DEV_FILES,
		  .steps = { { BYTES(0x14, 0xF6, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01), BYTES(0x94, 0x03) } } },
		{ "read: PDU longer than byte count", PDU_DEV_FILES,
		  .steps = { { BYTES(0x14, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00), BYTES(0x94, 0x03) } } },
		{ "read: reference type", PDU_DEV_FILES,
		  .steps = { { BYTES(0x14, 0x07, 0x05, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01), BYTES(0x94, 0x03) } } },
		{ "read: zero record length", PDU_DEV_FILES,
		  .steps = { { BYTES(0x14, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00), BYTES(0x94, 0x03) } } },
		{ "read: record number above 0x270F", PDU_DEV_FILES,
		  .steps = { { BYTES(0x14, 0x07, 0x06, 0x00, 0x01, 0x27, 0x10, 0x00, 0x01), BYTES(0x94, 0x03) } } },
		{ "read: range above 0x270F", PDU_DEV_FILES,
		  .steps = { { BYTES(0x14, 0x07, 0x06, 0x00, 0x01, 0x27, 0x0F, 0x00, 0x02), BYTES(0x94, 0x03) } } },
		{ "read: file number 0", PDU_DEV_FILES,
		  .steps = { { BYTES(0x14, 0x07, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01), BYTES(0x94, 0x03) } } },
		{ "read: response exceeds the PDU", PDU_DEV_FILES,
		  .steps = { { BYTES(0x14, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x7E), BYTES(0x94, 0x03) } } },
		/* FC 21 malformed */
		{ "write: missing sub-request", PDU_DEV_FILES, .steps = { { EXC(0x15, 0x03) } } },
		{ "write: byte count mismatch", PDU_DEV_FILES,
		  .steps = { { BYTES(0x15, 0x08, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01), BYTES(0x95, 0x03) } } },
		{ "write: byte count below minimum", PDU_DEV_FILES,
		  .steps = { { BYTES(0x15, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01), BYTES(0x95, 0x03) } } },
		{ "write: byte count above maximum", PDU_DEV_FILES,
		  .steps = { { BYTES(0x15, 0xFC, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01), BYTES(0x95, 0x03) } } },
		{ "write: PDU longer than byte count", PDU_DEV_FILES,
		  .steps = { { BYTES(0x15, 0x09, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00), BYTES(0x95, 0x03) } } },
		{ "write: reference type", PDU_DEV_FILES,
		  .steps = { { BYTES(0x15, 0x09, 0x05, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01), BYTES(0x95, 0x03) } } },
		{ "write: zero record length", PDU_DEV_FILES,
		  .steps = { { BYTES(0x15, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00), BYTES(0x95, 0x03) } } },
		{ "write: record number above 0x270F", PDU_DEV_FILES,
		  .steps = { { BYTES(0x15, 0x09, 0x06, 0x00, 0x01, 0x27, 0x10, 0x00, 0x01, 0x00, 0x01), BYTES(0x95, 0x03) } } },
		{ "write: range above 0x270F", PDU_DEV_FILES,
		  .steps = { { BYTES(0x15, 0x0B, 0x06, 0x00, 0x01, 0x27, 0x0F, 0x00, 0x02, 0x00, 0x01, 0x00, 0x02), BYTES(0x95, 0x03) } } },
		{ "write: file number 0", PDU_DEV_FILES,
		  .steps = { { BYTES(0x15, 0x09, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01), BYTES(0x95, 0x03) } } },
		{ "write: claims 2 registers, provides 1", PDU_DEV_FILES,
		  .steps = { { BYTES(0x15, 0x09, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01), BYTES(0x95, 0x03) } } },
		{ "write: trailing byte after a sub-request", PDU_DEV_FILES,
		  .steps = { { BYTES(0x15, 0x0A, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00), BYTES(0x95, 0x03) } } },
		/* unavailable records are address errors */
		{ "read: missing file", PDU_DEV_FILES,
		  .steps = { { BYTES(0x14, 0x07, 0x06, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01), BYTES(0x94, 0x02) } } },
		{ "read: missing record", PDU_DEV_FILES,
		  .steps = { { BYTES(0x14, 0x07, 0x06, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01), BYTES(0x94, 0x02) } } },
		{ "read: range spans an undefined record", PDU_DEV_FILES,
		  .steps = { { BYTES(0x14, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03), BYTES(0x94, 0x02) } } },
		{ "read: no file records configured", PDU_DEV_BASE,
		  .steps = { { BYTES(0x14, 0x07, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01), BYTES(0x94, 0x02) } } },
		{ "write: missing file", PDU_DEV_FILES,
		  .steps = { { BYTES(0x15, 0x09, 0x06, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01), BYTES(0x95, 0x02) } } },
		{ "write: missing record", PDU_DEV_FILES,
		  .steps = { { BYTES(0x15, 0x09, 0x06, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01), BYTES(0x95, 0x02) } } },
		{ "write: range spans an undefined record", PDU_DEV_FILES,
		  .steps = { { BYTES(0x15, 0x0D, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03),
			       BYTES(0x95, 0x02) } } },
		{ "write: no file records configured", PDU_DEV_BASE,
		  .steps = { { BYTES(0x15, 0x09, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01), BYTES(0x95, 0x02) } } },
	};

	run_pdu_cases(cases, ARRAY_COUNT(cases));
}

TEST(ReadFifoQueueAndReadWriteMultipleRegisters)
{
	static const pdu_case_t cases[] = {
		{ "fifo configured values", PDU_DEV_FIFOS,
		  .steps = { { BYTES(0x18, 0x00, 0x0A),
			       BYTES(0x18, 0x00, 0x08, 0x00, 0x03, 0x12, 0x34, 0x56, 0x78, 0x00, 0x03) } } },
		{ "fifo empty and reads are not destructive", PDU_DEV_FIFOS,
		  .steps = { { BYTES(0x18, 0x00, 0x01), BYTES(0x18, 0x00, 0x02, 0x00, 0x00) },
			     { BYTES(0x18, 0x00, 0x02), BYTES(0x18, 0x00, 0x06, 0x00, 0x02, 0x00, 0x01, 0x00, 0x02) },
			     { BYTES(0x18, 0x00, 0x02), BYTES(0x18, 0x00, 0x06, 0x00, 0x02, 0x00, 0x01, 0x00, 0x02) } } },
		{ "fifo without values", PDU_DEV_FIFOS,
		  .steps = { { BYTES(0x18, 0x00, 0x07), BYTES(0x18, 0x00, 0x02, 0x00, 0x00) } } },
		{ "rwm writes and reads back", PDU_DEV_BASE,
		  .steps = { { BYTES(0x17, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x04, 0xAA, 0xAA, 0xBB, 0xBB),
			       BYTES(0x17, 0x04, 0xAA, 0xAA, 0xBB, 0xBB) },
			     { BYTES(0x03, 0x00, 0x00, 0x00, 0x02), BYTES(0x03, 0x04, 0xAA, 0xAA, 0xBB, 0xBB) } } },
		/* read of 1,2,3 after the write to 0,1,2 gives 101,102,13 */
		{ "rwm write before read, overlapping", PDU_DEV_HOLD10,
		  .steps = { { BYTES(0x17, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x03, 0x06, 0x00, 0x64, 0x00, 0x65, 0x00, 0x66),
			       BYTES(0x17, 0x06, 0x00, 0x65, 0x00, 0x66, 0x00, 0x0D) },
			     { BYTES(0x03, 0x00, 0x00, 0x00, 0x04),
			       BYTES(0x03, 0x08, 0x00, 0x64, 0x00, 0x65, 0x00, 0x66, 0x00, 0x0D) } } },
		{ "rwm write before read, non-overlapping", PDU_DEV_HOLD1,
		  .steps = { { BYTES(0x17, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x04, 0x00, 0x0A, 0x00, 0x14),
			       BYTES(0x17, 0x04, 0x00, 0x03, 0x00, 0x04) },
			     { BYTES(0x03, 0x00, 0x00, 0x00, 0x04),
			       BYTES(0x03, 0x08, 0x00, 0x0A, 0x00, 0x14, 0x00, 0x03, 0x00, 0x04) } } },
	};

	run_pdu_cases(cases, ARRAY_COUNT(cases));
}

TEST(DiagnosticsReturnQueryDataEchoesRequest)
{
	static const u8 reqs[][7] = {
		{ FC_DIAGNOSTICS, 0x00, 0x00, 0x12, 0x34 },
		{ FC_DIAGNOSTICS, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78 },	/* arbitrary data field */
	};
	static const u32 lens[] = { 5, 7 };
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	u8		resp[MODBUS_MAX_PDU];

	for (u32 i = 0; i < ARRAY_COUNT(lens); i++)
	{
		ASSERT_EQ(run(dev, reqs[i], lens[i], resp) == lens[i]);
		ASSERT_EQ(memcmp(resp, reqs[i], lens[i]) == 0);
	}
	arena_release(arena);
}

TEST(DiagnosticsRestartCommunicationsClearsCountersAndEventLog)
{
	static const struct {
		const char*	name;
		u16		data;
		bool		keeps_log;
	} cases[] = {
		{ "0000 preserves the event log", 0x0000, true },
		{ "FF00 clears the event log",    0xFF00, false },
	};
	static const u8	log[] = { 0x11, 0x22, 0x33 };
	u8		fc12[] = { FC_GET_COM_EVENT_LOG };

	for (u32 i = 0; i < ARRAY_COUNT(cases); i++)
	{
		arena_t*	arena = ARENA_ALLOC();
		device_t*	dev   = make_device(arena);
		u8		req[5];
		u8		resp[MODBUS_MAX_PDU];
		u32		log_len = cases[i].keeps_log ? sizeof(log) : 0;

		fprintf(stderr, "restart %s\n", cases[i].name);
		dev->diagnostic_register    = DIAG_REG_CHARACTER_OVERRUN;
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

		diagnostic_request(req, DIAG_SUB_RESTART_COMMUNICATIONS, cases[i].data);
		ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(req));
		ASSERT_EQ(memcmp(resp, req, sizeof(req)) == 0);
		ASSERT_EQ(!dev->listen_only);
		ASSERT_EQ(dev->diagnostic_register == 0);
		ASSERT_EQ(dev->com_event_count == 0);
		ASSERT_EQ(dev->message_count == 0);
		ASSERT_EQ(dev->diag_comm_error_count == 0 && dev->diag_exception_count == 0);
		ASSERT_EQ(dev->diag_no_response_count == 0 && dev->diag_nak_count == 0);
		ASSERT_EQ(dev->diag_busy_count == 0 && dev->diag_overrun_count == 0);
		ASSERT_EQ(dev->com_event_log_count == log_len);
		if (cases[i].keeps_log)
			ASSERT_EQ(memcmp(dev->com_events, log, sizeof(log)) == 0);
		else
			for (u32 k = 0; k < DEVICE_MAX_COM_EVENTS; k++)
				ASSERT_EQ(dev->com_events[k] == 0);

		ASSERT_EQ(run(dev, fc12, sizeof(fc12), resp) == 8 + log_len);
		ASSERT_EQ(resp[1] == 6 + log_len);
		ASSERT_EQ(resp[4] == 0 && resp[5] == 0);
		ASSERT_EQ(resp[6] == 0 && resp[7] == 1);
		ASSERT_EQ(memcmp(resp + 8, log, log_len) == 0);
		arena_release(arena);
	}
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

typedef enum {
	MEI_DEV_BASIC,		/* default identification objects only */
	MEI_DEV_REGULAR,	/* + vendor url, product, model and application names */
	MEI_DEV_EXTENDED,	/* + url, product and extended objects 0x80, 0x90, 0xA0 */
	MEI_DEV_LONG_EXTENDED,	/* three 130-byte extended objects: only one fits a response */
	MEI_DEV_LONG_BASIC,	/* three 244-byte basic objects on an empty device */
} mei_dev_t;

TEST(ReadDeviceIdentificationStreamsAndSpecificObjects)
{
	typedef struct { u8 id; const char* value; } obj_t;
	static char	long130[131], long_v[245], long_p[245], long_r[245];
	static const obj_t objs[][8] = {
		[MEI_DEV_BASIC]    = { { 0x00, "mogbus" }, { 0x01, "mogbus" }, { 0x02, "1.0" } },
		[MEI_DEV_REGULAR]  = { { 0x00, "mogbus" }, { 0x01, "mogbus" }, { 0x02, "1.0" },
				       { 0x03, "https://example.invalid" }, { 0x04, "Simulator" },
				       { 0x05, "M1" }, { 0x06, "TestApp" } },
		[MEI_DEV_EXTENDED] = { { 0x00, "mogbus" }, { 0x01, "mogbus" }, { 0x02, "1.0" },
				       { 0x03, "url" }, { 0x04, "Sim" }, { 0x80, "Alpha" },
				       { 0x90, "Mike" }, { 0xA0, "Zulu" } },
		[MEI_DEV_LONG_EXTENDED] = { { 0x00, "mogbus" }, { 0x01, "mogbus" }, { 0x02, "1.0" },
					    { 0x80, long130 }, { 0x90, long130 }, { 0xA0, long130 } },
		[MEI_DEV_LONG_BASIC] = { { 0x00, long_v }, { 0x01, long_p }, { 0x02, long_r } },
	};
	/* conformity 0 = skip; exc != 0 = expect that exception; objects are a slice of objs[dev] */
	static const struct {
		const char*	name;
		mei_dev_t	dev;
		u8		code, obj, conformity, exc, more, next, from, n;
	} cases[] = {
		{ "basic stream", MEI_DEV_BASIC, 1, 0x00, 0x81, 0, 0x00, 0x00, 0, 3 },
		{ "specific, known but unconfigured object", MEI_DEV_BASIC, 4, 0x03, 0, EXC_ILLEGAL_DATA_ADDRESS, 0, 0, 0, 0 },
		{ "regular stream", MEI_DEV_REGULAR, 2, 0x00, 0x82, 0, 0x00, 0x00, 0, 7 },
		{ "extended stream from 3", MEI_DEV_REGULAR, 3, 0x03, 0x82, 0, 0x00, 0x00, 3, 4 },
		{ "specific object 1", MEI_DEV_REGULAR, 4, 0x01, 0x82, 0, 0x00, 0x00, 1, 1 },
		{ "specific object 4", MEI_DEV_REGULAR, 4, 0x04, 0x82, 0, 0x00, 0x00, 4, 1 },
		{ "extended: full stream", MEI_DEV_EXTENDED, 3, 0x00, 0x83, 0, 0x00, 0x00, 0, 8 },
		{ "extended: resume at 0x90", MEI_DEV_EXTENDED, 3, 0x90, 0x83, 0, 0x00, 0x00, 6, 2 },
		{ "extended: unknown start restarts at 0", MEI_DEV_EXTENDED, 3, 0x7F, 0, 0, 0x00, 0x00, 0, 8 },
		{ "extended: basic category", MEI_DEV_EXTENDED, 1, 0x00, 0x83, 0, 0x00, 0x00, 0, 3 },
		{ "extended: regular category", MEI_DEV_EXTENDED, 2, 0x00, 0x83, 0, 0x00, 0x00, 0, 5 },
		{ "extended: specific 0x90", MEI_DEV_EXTENDED, 4, 0x90, 0x83, 0, 0x00, 0x00, 6, 1 },
		{ "extended: specific absent object", MEI_DEV_EXTENDED, 4, 0x81, 0, EXC_ILLEGAL_DATA_ADDRESS, 0, 0, 0, 0 },
		{ "extended more follows 1", MEI_DEV_LONG_EXTENDED, 3, 0x00, 0x83, 0, 0xFF, 0x90, 0, 4 },
		{ "extended more follows 2", MEI_DEV_LONG_EXTENDED, 3, 0x90, 0, 0, 0xFF, 0xA0, 4, 1 },
		{ "extended more follows 3", MEI_DEV_LONG_EXTENDED, 3, 0xA0, 0, 0, 0x00, 0x00, 5, 1 },
		/* 7-byte MEI header + 2-byte object header + 244-byte value = 253 (MODBUS_MAX_PDU) */
		{ "more follows 1", MEI_DEV_LONG_BASIC, 1, 0x00, 0, 0, 0xFF, 0x01, 0, 1 },
		{ "more follows 2", MEI_DEV_LONG_BASIC, 1, 0x01, 0, 0, 0xFF, 0x02, 1, 1 },
		{ "more follows 3", MEI_DEV_LONG_BASIC, 1, 0x02, 0, 0, 0x00, 0x00, 2, 1 },
	};

	memset(long130, 'E', 130);
	memset(long_v, 'V', 244);
	memset(long_p, 'P', 244);
	memset(long_r, 'R', 244);
	for (u32 i = 0; i < ARRAY_COUNT(cases); i++)
	{
		arena_t*	arena = ARENA_ALLOC();
		device_t*	dev   = make_device(arena);
		u8		req[] = { FC_ENCAPSULATED_INTERFACE_TRANSPORT, 0x0E, cases[i].code, cases[i].obj };
		u8		resp[MODBUS_MAX_PDU];
		u32		off = 7;
		u32		len;

		switch (cases[i].dev)
		{
		case MEI_DEV_BASIC:
			break;
		case MEI_DEV_REGULAR:
			strcpy(dev->device_id[DEVICE_ID_VENDOR_URL], "https://example.invalid");
			strcpy(dev->device_id[DEVICE_ID_PRODUCT_NAME], "Simulator");
			strcpy(dev->device_id[DEVICE_ID_MODEL_NAME], "M1");
			strcpy(dev->device_id[DEVICE_ID_USER_APPLICATION_NAME], "TestApp");
			break;
		case MEI_DEV_EXTENDED:
			strcpy(dev->device_id[DEVICE_ID_VENDOR_URL], "url");
			strcpy(dev->device_id[DEVICE_ID_PRODUCT_NAME], "Sim");
			add_extended_id(dev, 0xA0, "Zulu");
			add_extended_id(dev, 0x80, "Alpha");
			add_extended_id(dev, 0x90, "Mike");
			break;
		case MEI_DEV_LONG_EXTENDED:
			add_extended_id(dev, 0x80, long130);
			add_extended_id(dev, 0x90, long130);
			add_extended_id(dev, 0xA0, long130);
			break;
		case MEI_DEV_LONG_BASIC:
			dev = device_new(arena, 0);
			strcpy(dev->device_id[DEVICE_ID_VENDOR_NAME], long_v);
			strcpy(dev->device_id[DEVICE_ID_PRODUCT_CODE], long_p);
			strcpy(dev->device_id[DEVICE_ID_MAJOR_MINOR_REVISION], long_r);
			break;
		}

		fprintf(stderr, "device id case '%s'\n", cases[i].name);
		len = run(dev, req, sizeof(req), resp);
		if (cases[i].exc != 0)
		{
			ASSERT_EQ(len == 2 && resp[0] == 0xAB && resp[1] == cases[i].exc);
			arena_release(arena);
			continue;
		}
		ASSERT_EQ(len <= MODBUS_MAX_PDU);
		ASSERT_EQ(resp[0] == FC_ENCAPSULATED_INTERFACE_TRANSPORT && resp[1] == 0x0E);
		ASSERT_EQ(resp[2] == cases[i].code);
		ASSERT_EQ(cases[i].conformity == 0 || resp[3] == cases[i].conformity);
		ASSERT_EQ(resp[4] == cases[i].more && resp[5] == cases[i].next);
		ASSERT_EQ(resp[6] == cases[i].n);
		for (u32 k = 0; k < cases[i].n; k++)
		{
			const obj_t* o = &objs[cases[i].dev][cases[i].from + k];

			assert_mei_object(resp, &off, o->id, o->value);
		}
		ASSERT_EQ(len == off);
		arena_release(arena);
	}
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
	assert_mei_object(resp, &off, 0x00, "mogbus");
	assert_mei_object(resp, &off, 0x01, "mogbus");
	assert_mei_object(resp, &off, 0x02, "1.0");

	off = 7;
	run(dev, regular, sizeof(regular), resp);
	ASSERT_EQ(resp[2] == 0x02);
	ASSERT_EQ(resp[3] == 0x82);
	ASSERT_EQ(resp[4] == 0x00);
	ASSERT_EQ(resp[6] == 0x04);
	assert_mei_object(resp, &off, 0x00, "mogbus");
	assert_mei_object(resp, &off, 0x01, "mogbus");
	assert_mei_object(resp, &off, 0x02, "1.0");
	assert_mei_object(resp, &off, 0x04, "Simulator");

	off = 7;
	run(dev, extended, sizeof(extended), resp);
	ASSERT_EQ(resp[2] == 0x03);
	ASSERT_EQ(resp[3] == 0x82);
	ASSERT_EQ(resp[4] == 0x00);
	ASSERT_EQ(resp[6] == 0x04);
	assert_mei_object(resp, &off, 0x00, "mogbus");
	assert_mei_object(resp, &off, 0x01, "mogbus");
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
	ASSERT_EQ(dev->message_count == 1 && dev->com_event_count == 0);
	ASSERT_EQ(dev->diag_exception_count == 1 && dev->diag_busy_count == 1);

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

TEST(InjectedExceptionsAreCountedByCode)
{
	static const struct {
		const char*	name;
		u8		code;
		bool		corrupt;
		u8		resp_code;
		u16		busy;
		u16		nak;
	} cases[] = {
		{ "busy",              EXC_SLAVE_DEVICE_BUSY,   false, EXC_SLAVE_DEVICE_BUSY, 1, 0 },
		{ "nak",               EXC_DEVICE_NAK,          false, EXC_DEVICE_NAK,        0, 1 },
		{ "other",             EXC_MEMORY_PARITY_ERROR, false, EXC_MEMORY_PARITY_ERROR, 0, 0 },
		/* corruption flips the reply but the counters keep the injected code */
		{ "busy then corrupt", EXC_SLAVE_DEVICE_BUSY,   true,  EXC_SLAVE_DEVICE_BUSY ^ 0xFF, 1, 0 },
	};
	u8 read[] = { FC_READ_HOLDING_REGISTERS, 0, 0, 0, 1 };
	u8 fc11[] = { FC_GET_COM_EVENT_COUNTER };

	for (u32 i = 0; i < ARRAY_COUNT(cases); i++)
	{
		arena_t*		arena = ARENA_ALLOC();
		device_t*		dev   = make_device(arena);
		rng_t			rng;
		fault_decision_t	decision = { 0 };
		u8			resp[MODBUS_MAX_PDU];

		fprintf(stderr, "injected exception case '%s'\n", cases[i].name);
		set_fault(dev, FAULT_EXCEPTION, 0, cases[i].code, 100);
		if (cases[i].corrupt)
			set_fault(dev, FAULT_CORRUPT, 0, 0, 100);
		rng_seed(&rng, 1);
		ASSERT_EQ(run_request(dev, &rng, read, sizeof(read), resp, &decision) == 2);
		ASSERT_EQ(resp[0] == (FC_READ_HOLDING_REGISTERS | 0x80));
		ASSERT_EQ(resp[1] == cases[i].resp_code && !decision.drop);
		ASSERT_EQ(run(dev, fc11, sizeof(fc11), resp) == 5);
		ASSERT_EQ(get_be16(resp + 3) == 0);
		assert_exception_counters(dev, 1, cases[i].busy, cases[i].nak);
		arena_release(arena);
	}
}

TEST(InjectedExceptionReplacesRealExceptionAccounting)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			bad_addr[] = { FC_READ_HOLDING_REGISTERS, 0x7F, 0xFF, 0, 1 };
	u8			resp[MODBUS_MAX_PDU];

	set_fault(dev, FAULT_EXCEPTION, 0, EXC_SLAVE_DEVICE_BUSY, 100);
	rng_seed(&rng, 1);
	ASSERT_EQ(run_request(dev, &rng, bad_addr, sizeof(bad_addr), resp, &decision) == 2);
	ASSERT_EQ(resp[1] == EXC_SLAVE_DEVICE_BUSY);
	assert_exception_counters(dev, 1, 1, 0);
	arena_release(arena);
}

TEST(InjectedExceptionOnRestartIsCounted)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			restart[5];
	u8			resp[MODBUS_MAX_PDU];

	set_fault(dev, FAULT_EXCEPTION, 0, EXC_SLAVE_DEVICE_BUSY, 100);
	rng_seed(&rng, 1);
	diagnostic_request(restart, DIAG_SUB_RESTART_COMMUNICATIONS, 0x0000);
	ASSERT_EQ(run_request(dev, &rng, restart, sizeof(restart), resp, &decision) == 2);
	ASSERT_EQ(resp[1] == EXC_SLAVE_DEVICE_BUSY && !decision.drop);
	ASSERT_EQ(dev->com_event_count == 0);
	assert_exception_counters(dev, 1, 1, 0);
	arena_release(arena);
}

TEST(InjectedExceptionInListenOnlyIsSilent)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			listen[5];
	u8			read[] = { FC_READ_HOLDING_REGISTERS, 0, 0, 0, 1 };
	u8			resp[MODBUS_MAX_PDU];

	diagnostic_request(listen, DIAG_SUB_LISTEN_ONLY_MODE, 0x0000);
	rng_seed(&rng, 1);
	ASSERT_EQ(run_request(dev, &rng, listen, sizeof(listen), resp, &decision) == 0);
	ASSERT_EQ(dev->listen_only && decision.drop);

	set_fault(dev, FAULT_EXCEPTION, 0, EXC_SLAVE_DEVICE_BUSY, 100);
	decision = (fault_decision_t){ 0 };
	ASSERT_EQ(run_request(dev, &rng, read, sizeof(read), resp, &decision) == 2);
	ASSERT_EQ(resp[1] == EXC_SLAVE_DEVICE_BUSY && decision.drop);
	ASSERT_EQ(!decision.disconnect && dev->diag_no_response_count == 2);

	dev->listen_only = false; /* test-only: allow counter reads while faulted */
	assert_exception_counters(dev, 0, 0, 0);
	arena_release(arena);
}

TEST(CorruptFaultDoesNotCountException)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			read[] = { FC_READ_HOLDING_REGISTERS, 0, 0, 0, 1 };
	u8			fc11[] = { FC_GET_COM_EVENT_COUNTER };
	u8			resp[MODBUS_MAX_PDU];

	set_fault(dev, FAULT_CORRUPT, 0, 0, 100);
	rng_seed(&rng, 1);
	ASSERT_EQ(run_request(dev, &rng, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS && resp[1] == (0x02 ^ 0xFF));
	ASSERT_EQ(run(dev, fc11, sizeof(fc11), resp) == 5);
	ASSERT_EQ(get_be16(resp + 3) == 1);
	assert_exception_counters(dev, 0, 0, 0);
	arena_release(arena);
}

TEST(RegisterCorruptFlipsRegisterValuesWithoutMutatingTables)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			read_holding[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x02 };
	u8			read_input[]   = { FC_READ_INPUT_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8			read_write[]   = { FC_READ_WRITE_MULTIPLE_REGISTERS,
						   0x00, 0x00, 0x00, 0x01,
						   0x00, 0x01, 0x00, 0x01,
						   0x02, 0x11, 0x22 };
	u8			write[]        = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x01, 0xAB, 0xCD };
	u8			coils[]        = { FC_READ_COILS, 0x00, 0x00, 0x00, 0x03 };
	u8			normal_coils[3];
	u8			resp[MODBUS_MAX_PDU];
	u16			out[2] = { 0, 0 };
	u32			len = 0;

	ASSERT_EQ(run(dev, coils, sizeof(coils), normal_coils) == 3);
	set_corrupt_fault(dev, FAULT_CORRUPT_REGISTER, 100);
	rng_seed(&rng, 31);

	len = run_request(dev, &rng, read_holding, sizeof(read_holding), resp, &decision);
	ASSERT_EQ(len == 6);
	ASSERT_EQ(decision.corrupt == FAULT_CORRUPT_REGISTER);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS && resp[1] == 0x04);
	ASSERT_EQ(resp[2] == (0x12 ^ 0xFF) && resp[3] == 0x34);
	ASSERT_EQ(resp[4] == 0x56 && resp[5] == 0x78);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 2, out));
	ASSERT_EQ(out[0] == 0x1234 && out[1] == 0x5678);

	len = run_request(dev, &rng, read_input, sizeof(read_input), resp, &decision);
	ASSERT_EQ(len == 4);
	ASSERT_EQ(resp[0] == FC_READ_INPUT_REGISTERS && resp[1] == 0x02);
	ASSERT_EQ(resp[2] == (0x00 ^ 0xFF) && resp[3] == 0x07);
	ASSERT_SUCCESS(device_read(dev, SPACE_INPUT, 0, 1, out));
	ASSERT_EQ(out[0] == 7);

	len = run_request(dev, &rng, read_write, sizeof(read_write), resp, &decision);
	ASSERT_EQ(len == 4);
	ASSERT_EQ(resp[2] == (0x12 ^ 0xFF) && resp[3] == 0x34);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 1, 1, out));
	ASSERT_EQ(out[0] == 0x1122);

	len = run_request(dev, &rng, write, sizeof(write), resp, &decision);
	ASSERT_EQ(len == 5 && resp[0] == FC_WRITE_SINGLE_REGISTER);
	ASSERT_EQ(resp[3] == (0xAB ^ 0xFF) && resp[4] == 0xCD);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 1, 1, out));
	ASSERT_EQ(out[0] == 0xABCD);

	len = run_request(dev, &rng, coils, sizeof(coils), resp, &decision);
	ASSERT_EQ(len == 3 && memcmp(resp, normal_coils, 3) == 0);
	arena_release(arena);
}

TEST(SensorDisconnectFailsInputReadsOnly)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			read_discrete[] = { FC_READ_DISCRETE_INPUTS, 0x00, 0x00, 0x00, 0x03 };
	u8			read_input[]    = { FC_READ_INPUT_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8			read_holding[]  = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8			bad_input[]     = { FC_READ_INPUT_REGISTERS, 0x7F, 0x00, 0x00, 0x01 };
	u8			resp[MODBUS_MAX_PDU];

	set_fault(dev, FAULT_SENSOR_DISCONNECT, 0, 0, 100);
	rng_seed(&rng, 32);

	ASSERT_EQ(run_request(dev, &rng, read_discrete, sizeof(read_discrete), resp,
			      &decision) == 2);
	ASSERT_EQ(resp[0] == (FC_READ_DISCRETE_INPUTS | 0x80));
	ASSERT_EQ(resp[1] == EXC_SLAVE_DEVICE_FAILURE);
	ASSERT_EQ(decision.sensor_disconnect);
	ASSERT_EQ(decision.exception_code == EXC_SLAVE_DEVICE_FAILURE);

	ASSERT_EQ(run_request(dev, &rng, read_input, sizeof(read_input), resp,
			      &decision) == 2);
	ASSERT_EQ(resp[0] == (FC_READ_INPUT_REGISTERS | 0x80));
	ASSERT_EQ(resp[1] == EXC_SLAVE_DEVICE_FAILURE);

	ASSERT_EQ(run_request(dev, &rng, read_holding, sizeof(read_holding), resp,
			      &decision) == 4);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS && resp[1] == 0x02);
	ASSERT_EQ(resp[2] == 0x12 && resp[3] == 0x34);
	ASSERT_EQ(decision.sensor_disconnect && decision.exception_code == 0);

	/* A real exception is never replaced by the sensor-disconnect fault. */
	ASSERT_EQ(run_request(dev, &rng, bad_input, sizeof(bad_input), resp, &decision) == 2);
	ASSERT_EQ(resp[0] == (FC_READ_INPUT_REGISTERS | 0x80));
	ASSERT_EQ(resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	ASSERT_EQ(decision.exception_code == 0);
	arena_release(arena);
}

TEST(SensorDisconnectYieldsToGenericException)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			read_input[] = { FC_READ_INPUT_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8			resp[MODBUS_MAX_PDU];

	set_fault(dev, FAULT_EXCEPTION, 0, EXC_GATEWAY_TARGET_FAILED, 100);
	set_fault(dev, FAULT_SENSOR_DISCONNECT, 0, 0, 100);
	rng_seed(&rng, 33);
	ASSERT_EQ(run_request(dev, &rng, read_input, sizeof(read_input), resp, &decision) == 2);
	ASSERT_EQ(resp[0] == (FC_READ_INPUT_REGISTERS | 0x80));
	ASSERT_EQ(resp[1] == EXC_GATEWAY_TARGET_FAILED);
	ASSERT_EQ(decision.exception_code == EXC_GATEWAY_TARGET_FAILED);
	ASSERT_EQ(decision.sensor_disconnect);
	arena_release(arena);
}

TEST(RestartFaultKeepsResponseAndResetsDeviceAfterwards)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			write[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0xAB, 0xCD };
	u8			read[]  = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8			resp[MODBUS_MAX_PDU];
	u16			out = 0;

	rng_seed(&rng, 34);
	ASSERT_EQ(run(dev, write, sizeof(write), resp) == 5);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 1, &out));
	ASSERT_EQ(out == 0xABCD);
	dev->diag_exception_count = 5;

	set_fault(dev, FAULT_RESTART, 0, 0, 100);
	ASSERT_EQ(run_request_at(dev, &rng, 2000, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS && resp[1] == 0x02);
	ASSERT_EQ(resp[2] == 0xAB && resp[3] == 0xCD);
	ASSERT_EQ(decision.restart && !decision.drop && !decision.disconnect);
	ASSERT_EQ(dev->start_ms == 2000);

	decision = (fault_decision_t){ 0 };
	ASSERT_EQ(run_request_at(dev, &rng, 3000, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(resp[2] == 0x12 && resp[3] == 0x34);
	ASSERT_EQ(decision.restart);
	ASSERT_EQ(dev->message_count == 3);
	ASSERT_EQ(dev->diag_exception_count == 5);
	arena_release(arena);
}

TEST(RestartFaultAppliesEvenWhenResponseIsDroppedOrDelayed)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			write[] = { FC_WRITE_SINGLE_REGISTER, 0x00, 0x00, 0xAB, 0xCD };
	u8			read[]  = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8			resp[MODBUS_MAX_PDU];
	u16			out = 0;

	rng_seed(&rng, 37);
	ASSERT_EQ(run(dev, write, sizeof(write), resp) == 5);
	set_fault(dev, FAULT_RESTART, 0, 0, 100);
	set_fault(dev, FAULT_DROP, 0, 0, 100);
	ASSERT_EQ(run_request(dev, &rng, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(decision.restart && decision.drop);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 1, &out));
	ASSERT_EQ(out == 0x1234);

	ASSERT_EQ(run(dev, write, sizeof(write), resp) == 5);
	dev->faults.present &= (u8)~FAULT_BIT(FAULT_DROP);
	set_fault(dev, FAULT_DELAY, 250, 0, 100);
	decision = (fault_decision_t){ 0 };
	ASSERT_EQ(run_request(dev, &rng, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(decision.restart && decision.delay_ms == 250 && !decision.drop);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 1, &out));
	ASSERT_EQ(out == 0x1234);
	arena_release(arena);
}

TEST(ScheduledFaultFiresThroughRepeatedRequests)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	const fault_schedule_step_t steps[] = { { .after_ms = 10, .count = 2 } };
	u8			read[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8			resp[MODBUS_MAX_PDU];

	set_scheduled_fault(dev, FAULT_DROP, steps, ARRAY_COUNT(steps), 41);
	rng_seed(&rng, 35);

	ASSERT_EQ(run_request_at(dev, &rng, 5, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(!decision.drop);
	ASSERT_EQ(run_request_at(dev, &rng, 10, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(decision.drop);
	ASSERT_EQ(run_request_at(dev, &rng, 11, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(decision.drop);
	ASSERT_EQ(run_request_at(dev, &rng, 12, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(!decision.drop);
	ASSERT_EQ(run_request_at(dev, &rng, 1000, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(!decision.drop);
	arena_release(arena);
}

TEST(ScheduledRestartFiresExactlyCountAndDoesNotRearm)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	const fault_schedule_step_t restart_steps[] = { { .after_ms = 10, .count = 2 } };
	const fault_schedule_step_t sensor_steps[]  = { { .after_ms = 10, .count = 2 } };
	u8			read[] = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	u8			resp[MODBUS_MAX_PDU];

	set_scheduled_fault(dev, FAULT_RESTART, restart_steps, ARRAY_COUNT(restart_steps), 42);
	set_scheduled_fault(dev, FAULT_SENSOR_DISCONNECT, sensor_steps,
			    ARRAY_COUNT(sensor_steps), 43);
	rng_seed(&rng, 36);

	ASSERT_EQ(run_request_at(dev, &rng, 5, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(!decision.restart && !decision.sensor_disconnect);

	ASSERT_EQ(run_request_at(dev, &rng, 10, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(decision.restart && decision.sensor_disconnect);

	/* The restart preserved its own progress, but reset the other kind. */
	ASSERT_EQ(run_request_at(dev, &rng, 10, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(decision.restart && !decision.sensor_disconnect);
	ASSERT_EQ(run_request_at(dev, &rng, 19, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(!decision.restart && !decision.sensor_disconnect);
	ASSERT_EQ(run_request_at(dev, &rng, 20, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(!decision.restart && decision.sensor_disconnect);
	ASSERT_EQ(run_request_at(dev, &rng, 21, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(!decision.restart && decision.sensor_disconnect);
	ASSERT_EQ(run_request_at(dev, &rng, 1000, read, sizeof(read), resp, &decision) == 4);
	ASSERT_EQ(!decision.restart && !decision.sensor_disconnect);
	arena_release(arena);
}

TEST(DroppedInjectedExceptionOnlyCountsNoResponse)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			read[] = { FC_READ_HOLDING_REGISTERS, 0, 0, 0, 1 };
	u8			req[5];
	u8			resp[MODBUS_MAX_PDU];

	set_fault(dev, FAULT_DROP, 0, 0, 100);
	set_fault(dev, FAULT_EXCEPTION, 0, EXC_SLAVE_DEVICE_BUSY, 100);
	rng_seed(&rng, 1);
	ASSERT_EQ(run_request(dev, &rng, read, sizeof(read), resp, &decision) == 2);
	ASSERT_EQ(decision.drop && !decision.disconnect);
	diagnostic_request(req, DIAG_SUB_RETURN_SLAVE_NO_RESPONSE_COUNT, 0);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 5);
	ASSERT_EQ(get_be16(resp + 3) == 1);
	assert_exception_counters(dev, 0, 0, 0);
	arena_release(arena);
}

TEST(WriteToMachineOwnedPointReturnsIllegalDataAddress)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = make_device(arena);
	u8 req[] = { FC_WRITE_SINGLE_REGISTER, 0, 0, 0xAB, 0xCD };
	u8 resp[MODBUS_MAX_PDU] = { 0 };
	u16 value = 0;

	dev->points[0].kind = POINT_MACHINE;
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 2);
	ASSERT_EQ(resp[0] == (FC_WRITE_SINGLE_REGISTER | 0x80) &&
		  resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 1, &value));
	ASSERT_EQ(value == 0x1234);
	arena_release(arena);
}

TEST(StateFaultOverlayReplacesDeviceFaultInPduResponse)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	machine_def_t*		def   = ARENA_PUSH_STRUCT(arena, machine_def_t);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			read[] = { FC_READ_HOLDING_REGISTERS, 0, 0, 0, 1 };
	u8			resp[MODBUS_MAX_PDU] = { 0 };

	ASSERT_SUCCESS(def != NULL);
	memset(def, 0, sizeof(*def));
	snprintf(def->name, sizeof(def->name), "faulted");
	def->states[0].name[0] = 'i';
	def->state_count = 1;
	def->states[0].faults.by_kind[FAULT_EXCEPTION] = (fault_t){
		.exception_code = EXC_SLAVE_DEVICE_BUSY, .percent = 100,
	};
	def->states[0].faults.present |= FAULT_BIT(FAULT_EXCEPTION);
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	set_fault(dev, FAULT_EXCEPTION, 0, EXC_SLAVE_DEVICE_FAILURE, 100);
	set_fault(dev, FAULT_DELAY, 12, 0, 100);
	ASSERT_SUCCESS(device_finalize_machines(dev) == NULL);

	rng_seed(&rng, 1);
	ASSERT_EQ(run_request(dev, &rng, read, sizeof(read), resp, &decision) == 2);
	ASSERT_EQ(decision.exception_code == EXC_SLAVE_DEVICE_BUSY);
	ASSERT_EQ(resp[1] == EXC_SLAVE_DEVICE_BUSY);
	ASSERT_EQ(decision.delay_ms == 12); /* unmentioned kinds keep the device fault */
	arena_release(arena);
}

TEST(WriteMultipleRegistersWithMachinePointFailsAllOrNothing)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = make_device(arena);
	machine_def_t* def = ARENA_PUSH_STRUCT(arena, machine_def_t);
	u8 req[] = { FC_WRITE_MULTIPLE_REGISTERS, 0, 0, 0, 2, 4, 0, 0xAA, 0, 0xBB };
	u8 resp[MODBUS_MAX_PDU] = { 0 };
	u16 out[2] = { 0, 0 };

	ASSERT_SUCCESS(def != NULL);
	memset(def, 0, sizeof(*def));
	snprintf(def->name, sizeof(def->name), "watched");
	def->state_count = 1;
	snprintf(def->states[0].name, sizeof(def->states[0].name), "idle");
	def->var_count = 1;
	snprintf(def->vars[0].name, sizeof(def->vars[0].name), "seen");
	def->vars[0].initial = (val_t){ .type = VAL_NUM, .n = 0 };
	def->nodes[0] = (expr_node_t){
		.kind = EXPR_NUM, .a = EXPR_NO_CHILD, .b = EXPR_NO_CHILD, .c = EXPR_NO_CHILD,
		.literal = { .type = VAL_NUM, .n = 99 },
	};
	def->node_count = 1;
	def->statements[0] = (machine_stmt_t){
		.kind = STMT_SET, .target_kind = TARGET_VAR, .var_index = 0,
		.expr_root = 0, .first_child = EXPR_NO_CHILD,
	};
	def->statement_count = 1;
	def->write_handlers[0] = (machine_write_handler_t){
		.first = 0, .count = 1, .space = SPACE_HOLDING, .address = 0,
	};
	def->write_handler_count = 1;
	dev->points[1].kind = POINT_MACHINE;
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	ASSERT_SUCCESS(device_finalize_machines(dev) == NULL);

	ASSERT_EQ(run(dev, req, sizeof(req), resp) == 2);
	ASSERT_EQ(resp[0] == (FC_WRITE_MULTIPLE_REGISTERS | 0x80) &&
		  resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 2, out));
	ASSERT_EQ(out[0] == 0x1234 && out[1] == 0x5678);
	ASSERT_EQ(dev->machines->vars[0][0].n == 0); /* rejected write runs no on-write handler */
	arena_release(arena);
}

TEST(StateRegisterIsReadOnlyToModbusClients)
{
	arena_t* arena = ARENA_ALLOC();
	device_t* dev = make_device(arena);
	machine_def_t* def = ARENA_PUSH_STRUCT(arena, machine_def_t);
	u8 read[] = { FC_READ_HOLDING_REGISTERS, 0, 1, 0, 1 };
	u8 write[] = { FC_WRITE_SINGLE_REGISTER, 0, 1, 0xAB, 0xCD };
	u8 resp[MODBUS_MAX_PDU] = { 0 };
	u16 value = 0;

	ASSERT_SUCCESS(def != NULL);
	memset(def, 0, sizeof(*def));
	snprintf(def->name, sizeof(def->name), "state-view");
	def->state_count = 1;
	snprintf(def->states[0].name, sizeof(def->states[0].name), "idle");
	def->has_state_register = true;
	def->state_register_space = SPACE_HOLDING;
	def->state_register_address = 1;
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	ASSERT_SUCCESS(device_finalize_machines(dev) == NULL);

	ASSERT_EQ(run(dev, read, sizeof(read), resp) == 4);
	ASSERT_EQ(resp[0] == FC_READ_HOLDING_REGISTERS &&
		  resp[1] == 2 && resp[2] == 0 && resp[3] == 0);

	ASSERT_EQ(run(dev, write, sizeof(write), resp) == 2);
	ASSERT_EQ(resp[0] == (FC_WRITE_SINGLE_REGISTER | 0x80) &&
		  resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 1, 1, &value));
	ASSERT_EQ(value == 0);
	arena_release(arena);
}

TEST(OnWriteHandlerRunsInSuccessfulModbusWriteRequest)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	machine_def_t*		def   = ARENA_PUSH_STRUCT(arena, machine_def_t);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			write[] = { FC_WRITE_SINGLE_REGISTER, 0, 0, 0xAB, 0xCD };
	u8			resp[MODBUS_MAX_PDU] = { 0 };
	u16			written = 0;
	u16			side_effect = 0;

	ASSERT_SUCCESS(def != NULL);
	memset(def, 0, sizeof(*def));
	snprintf(def->name, sizeof(def->name), "on-write");
	def->state_count = 2;
	snprintf(def->states[0].name, sizeof(def->states[0].name), "idle");
	snprintf(def->states[1].name, sizeof(def->states[1].name), "later");
	def->transitions[0] = (machine_transition_def_t){
		.from = 0, .to = 1, .after_kind = AFTER_LITERAL, .after_literal_ms = 1,
	};
	def->transition_count = 1;
	def->nodes[0] = (expr_node_t){
		.kind = EXPR_POINT, .a = EXPR_NO_CHILD, .b = EXPR_NO_CHILD, .c = EXPR_NO_CHILD,
		.space = SPACE_HOLDING, .address = 0,
	};
	def->node_count = 1;
	def->statements[0] = (machine_stmt_t){
		.kind = STMT_SET, .target_kind = TARGET_POINT,
		.space = SPACE_HOLDING, .address = 1,
		.expr_root = 0, .first_child = EXPR_NO_CHILD,
	};
	def->statement_count = 1;
	def->write_handlers[0] = (machine_write_handler_t){
		.first = 0, .count = 1, .space = SPACE_HOLDING, .address = 0,
	};
	def->write_handler_count = 1;
	ASSERT_SUCCESS(device_add_machine(dev, def) == NULL);
	ASSERT_SUCCESS(device_finalize_machines(dev) == NULL);

	rng_seed(&rng, 1);
	ASSERT_EQ(modbus_handle_request(dev, &rng, 0, write, sizeof(write), resp, &decision) == 5);
	ASSERT_EQ(memcmp(resp, write, sizeof(write)) == 0); /* normal echo response */
	ASSERT_EQ(decision.exception_code == 0 && !decision.drop &&
		  !decision.disconnect && !decision.corrupt);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 1, &written));
	ASSERT_EQ(written == 0xABCD);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 1, 1, &side_effect));
	ASSERT_EQ(side_effect == 0xABCD); /* handler saw the just-written value */
	ASSERT_EQ(dev->machines->current[0] == 0);
	ASSERT_EQ(dev->machines->advanced_to[0] == 0);
	ASSERT_EQ(dev->machines->entered_at[0] == 0);
	arena_release(arena);
}

TEST(SilentRequestsDoNotConsumeFaultScheduleSteps)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev   = make_device(arena);
	rng_t			rng;
	fault_decision_t	decision = { 0 };
	u8			resp[MODBUS_MAX_PDU];
	u8			listen[] = { FC_DIAGNOSTICS, 0x00, 0x04, 0x00, 0x00 };
	u8			read[]   = { FC_READ_HOLDING_REGISTERS, 0x00, 0x00, 0x00, 0x01 };
	fault_schedule_step_t	steps[]  = { { .after_ms = 0, .count = 1 } };

	rng_seed(&rng, 5);
	ASSERT_EQ(modbus_handle_request(dev, &rng, 0, listen, sizeof(listen), resp, &decision) == 0);
	set_scheduled_fault(dev, FAULT_EXCEPTION, steps, 1, 1);
	dev->faults.by_kind[FAULT_EXCEPTION].exception_code = EXC_ACKNOWLEDGE;
	ASSERT_EQ(modbus_handle_request(dev, &rng, 0, read, sizeof(read), resp, &decision) == 2);
	ASSERT_EQ(decision.drop);
	dev->listen_only = false;
	ASSERT_EQ(modbus_handle_request(dev, &rng, 0, read, sizeof(read), resp, &decision) == 2);
	ASSERT_EQ(resp[0] == 0x83 && resp[1] == 0x05);
	arena_release(arena);
}

/* User-defined function code 0x41 on make_device(): holding 0-1 = 0x1234 0x5678, coils 0-9 off. */
static device_t*
make_custom_fc_device(arena_t* arena)
{
	device_t*		dev = make_device(arena);
	device_custom_fc_t	fc  = { .code = 0x41 };

	fc.cases[0] = (struct device_custom_case_t){ .prefix = { 0x01 }, .prefix_len = 1,
		.action = CUSTOM_REPLY, .reply = { 0x00, 0xAB, 0xCD }, .reply_len = 3 };
	fc.cases[1] = (struct device_custom_case_t){ .prefix = { 0x02 }, .prefix_len = 1,
		.action = CUSTOM_REGISTERS, .space = SPACE_HOLDING, .address = 0, .count = 2 };
	fc.cases[2] = (struct device_custom_case_t){ .prefix = { 0x03 }, .prefix_len = 1,
		.action = CUSTOM_REGISTERS, .space = SPACE_COIL, .address = 0, .count = 10 };
	fc.cases[3] = (struct device_custom_case_t){ .prefix = { 0x04, 0x05 }, .prefix_len = 2,
		.action = CUSTOM_ECHO };
	fc.cases[4] = (struct device_custom_case_t){ .prefix = { 0x04 }, .prefix_len = 1,
		.action = CUSTOM_EXCEPTION, .exception_code = 0x04 };
	fc.cases[5] = (struct device_custom_case_t){ .prefix = { 0x06 }, .prefix_len = 1,
		.action = CUSTOM_NO_RESPONSE };
	fc.cases[6] = (struct device_custom_case_t){ .prefix = { 0x07 }, .prefix_len = 1,
		.action = CUSTOM_REGISTERS, .space = SPACE_HOLDING, .address = 500, .count = 1 };
	fc.case_count = 7;
	ASSERT_EQ(device_add_custom_fc(dev, &fc) == NULL);
	return dev;
}

static u32
custom_call(device_t* dev, const u8* req, u32 len, u8* resp)
{
	return modbus_handle_pdu(dev, 0, req, len, resp);
}

TEST(UserDefinedFunctionCodeActions)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev = make_custom_fc_device(arena);
	u8		resp[MODBUS_MAX_PDU];
	const u8	reply[]  = { 0x41, 0x01 };
	const u8	regs[]   = { 0x41, 0x02 };
	const u8	bits[]   = { 0x41, 0x03 };
	const u8	echo[]   = { 0x41, 0x04, 0x05, 0xEE };
	const u8	exc[]    = { 0x41, 0x04, 0x99 };	/* "04 05" does not match: falls to "04" */
	const u8	silent[] = { 0x41, 0x06 };
	const u8	undef[]  = { 0x41, 0x07 };
	const u8	nomatch[] = { 0x41, 0x09 };
	const u8	empty[]  = { 0x41 };
	u32		len;

	len = custom_call(dev, reply, sizeof(reply), resp);
	ASSERT_EQ(len == 4 && resp[0] == 0x41 && resp[1] == 0x00 && resp[2] == 0xAB && resp[3] == 0xCD);
	len = custom_call(dev, regs, sizeof(regs), resp);
	ASSERT_EQ(len == 6 && resp[1] == 4 && get_be16(resp + 2) == 0x1234 && get_be16(resp + 4) == 0x5678);
	ASSERT_EQ(device_write(dev, SPACE_COIL, 9, 1, (const u16[]){ 1 }));
	len = custom_call(dev, bits, sizeof(bits), resp);
	ASSERT_EQ(len == 4 && resp[1] == 2 && resp[2] == 0x00 && resp[3] == 0x02);	/* coil 9 = bit 1 of byte 2 */
	len = custom_call(dev, echo, sizeof(echo), resp);
	ASSERT_EQ(len == sizeof(echo) && memcmp(resp, echo, sizeof(echo)) == 0);
	len = custom_call(dev, exc, sizeof(exc), resp);
	ASSERT_EQ(len == 2 && resp[0] == 0xC1 && resp[1] == 0x04);
	ASSERT_EQ(custom_call(dev, silent, sizeof(silent), resp) == 0);
	len = custom_call(dev, undef, sizeof(undef), resp);
	ASSERT_EQ(len == 2 && resp[0] == 0xC1 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	len = custom_call(dev, nomatch, sizeof(nomatch), resp);
	ASSERT_EQ(len == 2 && resp[0] == 0xC1 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	len = custom_call(dev, empty, sizeof(empty), resp);
	ASSERT_EQ(len == 2 && resp[1] == EXC_ILLEGAL_DATA_VALUE);
	arena_release(arena);
}

TEST(UserDefinedFunctionCodesKeepBuiltinsAndUnknownCodes)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev = make_custom_fc_device(arena);
	device_custom_fc_t	shadow = { .code = FC_READ_HOLDING_REGISTERS, .case_count = 1 };
	u8			resp[MODBUS_MAX_PDU];
	const u8		read[] = { FC_READ_HOLDING_REGISTERS, 0, 0, 0, 1 };
	const u8		other[] = { 0x42, 0x01 };
	u32			len;

	/* Config rejects redefining a built-in code; even if a device had one, the built-in wins. */
	shadow.cases[0] = (struct device_custom_case_t){ .action = CUSTOM_ECHO };
	ASSERT_EQ(device_add_custom_fc(dev, &shadow) == NULL);
	len = custom_call(dev, read, sizeof(read), resp);
	ASSERT_EQ(len == 4 && resp[0] == FC_READ_HOLDING_REGISTERS && get_be16(resp + 2) == 0x1234);
	len = custom_call(dev, other, sizeof(other), resp);
	ASSERT_EQ(len == 2 && resp[0] == 0xC2 && resp[1] == EXC_ILLEGAL_FUNCTION);
	ASSERT_EQ(device_add_custom_fc(dev, &shadow) != NULL);	/* duplicate code */
	arena_release(arena);
}

TEST(UserDefinedFunctionCodesGoThroughFaultsAndCounters)
{
	arena_t*		arena = ARENA_ALLOC();
	device_t*		dev = make_custom_fc_device(arena);
	u8			resp[MODBUS_MAX_PDU];
	const u8		reply[] = { 0x41, 0x01 };
	const u8		nomatch[] = { 0x41, 0x09 };
	fault_decision_t	decision = { 0 };
	rng_t			rng;
	u16			events = dev->com_event_count;
	u32			len;

	rng_seed(&rng, 1);
	(void)custom_call(dev, reply, sizeof(reply), resp);
	ASSERT_EQ(dev->com_event_count == (u16)(events + 1));
	(void)custom_call(dev, nomatch, sizeof(nomatch), resp);
	ASSERT_EQ(dev->diag_exception_count == 1);

	set_fault(dev, FAULT_EXCEPTION, 0, EXC_DEVICE_BUSY, 100);
	len = modbus_handle_request(dev, &rng, 0, reply, sizeof(reply), resp, &decision);
	ASSERT_EQ(len == 2 && resp[0] == 0xC1 && resp[1] == EXC_DEVICE_BUSY);
	arena_release(arena);
}

TEST(ReadHoldingRegistersReturnsSixtyFourBitAndStringBytes)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_device(arena);
	point_t		p     = { .space = SPACE_HOLDING, .kind = POINT_STATIC, .type = VT_U64,
				  .address = 100, .static_exact = true,
				  .static_raw = 0x0102030405060708ull };
	u8		req[] = { 0x03, 0x00, 100, 0x00, 0x04 };
	u8		resp[MODBUS_MAX_PDU];
	u8		want64[] = { 0x03, 0x08, 1, 2, 3, 4, 5, 6, 7, 8 };
	u8		want_le[] = { 0x03, 0x08, 2, 1, 4, 3, 6, 5, 8, 7 };
	u8		want_str[] = { 0x03, 0x06, 'A', 'B', 'C', 'D', 'E', 0 };

	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(want64));
	ASSERT_EQ(memcmp(resp, want64, sizeof(want64)) == 0);

	p.address    = 110;
	p.byte_order = BYTES_LITTLE;
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	req[2] = 110;
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(want_le));
	ASSERT_EQ(memcmp(resp, want_le, sizeof(want_le)) == 0);

	p = (point_t){ .space = SPACE_HOLDING, .kind = POINT_STATIC, .type = VT_STRING,
		       .address = 120, .length = 3, .text = "ABCDE" };
	ASSERT_EQ(device_add_point(dev, &p) == NULL);
	req[2] = 120;
	req[4] = 3;
	ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(want_str));
	ASSERT_EQ(memcmp(resp, want_str, sizeof(want_str)) == 0);

	/* A client writes the text with FC 16, then reads it back. */
	{
		u8	write[] = { 0x10, 0x00, 120, 0x00, 0x03, 0x06, 'H', 'i', 0, 0, 0, 0 };
		u8	want_hi[] = { 0x03, 0x06, 'H', 'i', 0, 0, 0, 0 };
		char	text[8];

		ASSERT_EQ(run(dev, write, sizeof(write), resp) == 5);
		ASSERT_EQ(run(dev, req, sizeof(req), resp) == sizeof(want_hi));
		ASSERT_EQ(memcmp(resp, want_hi, sizeof(want_hi)) == 0);
		ASSERT_SUCCESS(device_get_string(dev, SPACE_HOLDING, 120, text, sizeof(text)));
		ASSERT_EQ(strcmp(text, "Hi") == 0);
	}
	arena_release(arena);
}

/* Holding 0 = 100 (read-only), holding 1 = 200, holding 2 = 300 (read-only), holding 3 = 400;
 * coil 0 = on (read-only), coil 1 = off, coil 2 = off (read-only), coil 3 = off;
 * holding 10 ramps 0..1000 every 1000 ms (read-only). */
static device_t*
make_read_only_device(arena_t* arena)
{
	device_t*	dev = device_new(arena, 0);
	point_t		p   = { 0 };

	p.kind  = POINT_STATIC;
	p.space = SPACE_HOLDING;
	p.type  = VT_U16;
	for (u32 i = 0; i < 4; i++)
	{
		p.address      = i;
		p.static_value = 100 * (i + 1);
		p.read_only    = i % 2 == 0;
		ASSERT_SUCCESS(device_add_point(dev, &p) == NULL);
	}
	p.space = SPACE_COIL;
	p.type  = VT_BOOL;
	for (u32 i = 0; i < 4; i++)
	{
		p.address      = i;
		p.static_value = i == 0;
		p.read_only    = i % 2 == 0;
		ASSERT_SUCCESS(device_add_point(dev, &p) == NULL);
	}
	p.space     = SPACE_HOLDING;
	p.type      = VT_U16;
	p.kind      = POINT_RAMP;
	p.address   = 10;
	p.read_only = true;
	p.ramp      = (ramp_params_t){ .from = 0, .to = 1000, .step = 1, .every_ms = 1000, .mode = RAMP_HOLD };
	ASSERT_SUCCESS(device_add_point(dev, &p) == NULL);
	return dev;
}

static void
assert_regs(device_t* dev, u16 a, u16 b, u16 c, u16 d)
{
	u16	out[4];

	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 4, out));
	ASSERT_EQ(out[0] == a && out[1] == b && out[2] == c && out[3] == d);
}

TEST(ReadOnlyHoldingRefusesEveryClientWrite)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_read_only_device(arena);
	u8		resp[MODBUS_MAX_PDU];
	u8		fc06[]   = { 0x06, 0x00, 0x00, 0x00, 0x09 };
	u8		fc16[]   = { 0x10, 0x00, 0x01, 0x00, 0x02, 0x04, 0x00, 0x09, 0x00, 0x08 };	/* 1 and 2 */
	u8		fc16ok[] = { 0x10, 0x00, 0x03, 0x00, 0x01, 0x02, 0x00, 0x09 };
	u8		fc22[]   = { 0x16, 0x00, 0x02, 0x00, 0x00, 0x00, 0x07 };
	u8		fc22ok[] = { 0x16, 0x00, 0x01, 0x00, 0xF0, 0x00, 0x0F };
	u8		fc23[]   = { 0x17, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x02, 0x00, 0x09 };
	u8		fc23ok[] = { 0x17, 0x00, 0x00, 0x00, 0x02, 0x00, 0x03, 0x00, 0x01, 0x02, 0x00, 0x09 };
	u8		fc23rd[] = { 0x17, 0x00, 0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x01, 0x02, 0x00, 0x08 };

	ASSERT_EQ(run(dev, fc06, sizeof(fc06), resp) == 2);
	ASSERT_EQ(resp[0] == 0x86 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	assert_regs(dev, 100, 200, 300, 400);

	/* One read-only register in the middle of the range rejects the whole write. */
	ASSERT_EQ(run(dev, fc16, sizeof(fc16), resp) == 2);
	ASSERT_EQ(resp[0] == 0x90 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	assert_regs(dev, 100, 200, 300, 400);

	ASSERT_EQ(run(dev, fc22, sizeof(fc22), resp) == 2);
	ASSERT_EQ(resp[0] == 0x96 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	assert_regs(dev, 100, 200, 300, 400);

	/* FC 23: the write range is checked like an undefined one, nothing is written or read. */
	ASSERT_EQ(run(dev, fc23, sizeof(fc23), resp) == 2);
	ASSERT_EQ(resp[0] == 0x97 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	assert_regs(dev, 100, 200, 300, 400);

	/* Reads are unaffected, and so are the writable neighbours. */
	ASSERT_EQ(run(dev, fc16ok, sizeof(fc16ok), resp) == 5);
	ASSERT_EQ(run(dev, fc22ok, sizeof(fc22ok), resp) == 7);
	ASSERT_EQ(run(dev, fc23ok, sizeof(fc23ok), resp) == 6);
	ASSERT_EQ(resp[2] == 0 && resp[3] == 100 && resp[4] == 0 && resp[5] == 207);
	assert_regs(dev, 100, 207, 300, 9);

	/* A read-only register may be read with FC 23 while another range is written. */
	ASSERT_EQ(run(dev, fc23rd, sizeof(fc23rd), resp) == 10);
	assert_regs(dev, 100, 8, 300, 9);
	arena_release(arena);
}

TEST(ReadOnlyCoilRefusesClientWrites)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_read_only_device(arena);
	u8		resp[MODBUS_MAX_PDU];
	u8		fc05[]   = { 0x05, 0x00, 0x00, 0x00, 0x00 };
	u8		fc05b[]  = { 0x05, 0x00, 0x01, 0xFF, 0x00 };
	u8		fc15[]   = { 0x0F, 0x00, 0x01, 0x00, 0x02, 0x01, 0x03 };	/* coils 1 and 2 on */
	u8		fc15ok[] = { 0x0F, 0x00, 0x03, 0x00, 0x01, 0x01, 0x01 };
	u16		out[4];

	ASSERT_EQ(run(dev, fc05, sizeof(fc05), resp) == 2);
	ASSERT_EQ(resp[0] == 0x85 && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	ASSERT_EQ(run(dev, fc15, sizeof(fc15), resp) == 2);
	ASSERT_EQ(resp[0] == 0x8F && resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	ASSERT_SUCCESS(device_read(dev, SPACE_COIL, 0, 4, out));
	ASSERT_EQ(out[0] == 1 && out[1] == 0 && out[2] == 0 && out[3] == 0);

	ASSERT_EQ(run(dev, fc05b, sizeof(fc05b), resp) == 5);
	ASSERT_EQ(run(dev, fc15ok, sizeof(fc15ok), resp) == 5);
	ASSERT_SUCCESS(device_read(dev, SPACE_COIL, 0, 4, out));
	ASSERT_EQ(out[0] == 1 && out[1] == 1 && out[2] == 0 && out[3] == 1);
	arena_release(arena);
}

TEST(ReadOnlyRefusalCountsAsException)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_read_only_device(arena);
	u8		resp[MODBUS_MAX_PDU];
	u8		fc06[] = { 0x06, 0x00, 0x00, 0x00, 0x09 };

	ASSERT_EQ(run(dev, fc06, sizeof(fc06), resp) == 2);
	assert_exception_counters(dev, 1, 0, 0);
	arena_release(arena);
}

TEST(ReadOnlyRampKeepsMovingAfterRefusedWrite)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_read_only_device(arena);
	u8		resp[MODBUS_MAX_PDU];
	u8		fc06[] = { 0x06, 0x00, 0x0A, 0x00, 0x09 };
	u16		out = 0;

	device_tick(dev, 3000);
	ASSERT_EQ(run(dev, fc06, sizeof(fc06), resp) == 2);
	ASSERT_EQ(resp[1] == EXC_ILLEGAL_DATA_ADDRESS);
	device_tick(dev, 7000);
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 10, 1, &out));
	ASSERT_EQ(out == 7);
	arena_release(arena);
}

TEST(HostWriteIgnoresReadOnly)
{
	arena_t*	arena = ARENA_ALLOC();
	device_t*	dev   = make_read_only_device(arena);
	u16		value = 42;
	u16		out   = 0;

	ASSERT_SUCCESS(device_write(dev, SPACE_HOLDING, 0, 1, &value));
	ASSERT_SUCCESS(device_read(dev, SPACE_HOLDING, 0, 1, &out));
	ASSERT_EQ(out == 42);
	ASSERT_FAILURE(device_client_write(dev, SPACE_HOLDING, 0, 1, &value));
	ASSERT_SUCCESS(device_client_write(dev, SPACE_HOLDING, 1, 1, &value));
	arena_release(arena);
}
