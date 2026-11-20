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
