/* Public value helpers (mogbus_registers_to_value() and friends) against known vectors. */
#include "vtest.h"
#include "tests/tests_utils/test_case.h"
#include "mogbus.h"
#include <math.h>
#include <string.h>

static const mogbus_word_order_t WORD_ORDERS[] = { MOGBUS_WORDS_HIGH_FIRST, MOGBUS_WORDS_LOW_FIRST };
static const mogbus_byte_order_t BYTE_ORDERS[] = { MOGBUS_BYTES_BIG, MOGBUS_BYTES_LITTLE };

/* The four layouts of 0x0A0B0C0D (the table at mogbus_point_t) and of float 1.0 (0x3F800000). */
static const struct {
	const char*		name;
	mogbus_word_order_t	word;
	mogbus_byte_order_t	byte;
	uint16_t		bits[2];
	uint16_t		one[2];
} LAYOUTS[] = {
	{ "ABCD", MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG,    { 0x0A0B, 0x0C0D }, { 0x3F80, 0x0000 } },
	{ "CDAB", MOGBUS_WORDS_LOW_FIRST,  MOGBUS_BYTES_BIG,    { 0x0C0D, 0x0A0B }, { 0x0000, 0x3F80 } },
	{ "BADC", MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_LITTLE, { 0x0B0A, 0x0D0C }, { 0x803F, 0x0000 } },
	{ "DCBA", MOGBUS_WORDS_LOW_FIRST,  MOGBUS_BYTES_LITTLE, { 0x0D0C, 0x0B0A }, { 0x0000, 0x803F } },
};

TEST(ValueHelpersMatchTheFourFloatAndInt32Layouts)
{
	for (size_t i = 0; i < sizeof(LAYOUTS) / sizeof(LAYOUTS[0]); i++)
	{
		double		got = 0;
		uint16_t	regs[4] = { 0 };
		size_t		count = 0;

		ASSERT_EQ(mogbus_registers_to_value(LAYOUTS[i].bits, MOGBUS_TYPE_U32, LAYOUTS[i].word, LAYOUTS[i].byte, &got) == MOGBUS_OK);
		ASSERT_EQ(got == (double)0x0A0B0C0Du);
		ASSERT_EQ(mogbus_registers_to_value(LAYOUTS[i].one, MOGBUS_TYPE_F32, LAYOUTS[i].word, LAYOUTS[i].byte, &got) == MOGBUS_OK);
		ASSERT_EQ(got == 1.0);

		ASSERT_EQ(mogbus_value_to_registers((double)0x0A0B0C0Du, MOGBUS_TYPE_U32, LAYOUTS[i].word, LAYOUTS[i].byte, regs, &count) == MOGBUS_OK);
		ASSERT_EQ(count == 2 && regs[0] == LAYOUTS[i].bits[0] && regs[1] == LAYOUTS[i].bits[1]);
		ASSERT_EQ(mogbus_value_to_registers(1.0, MOGBUS_TYPE_F32, LAYOUTS[i].word, LAYOUTS[i].byte, regs, &count) == MOGBUS_OK);
		ASSERT_EQ(count == 2 && regs[0] == LAYOUTS[i].one[0] && regs[1] == LAYOUTS[i].one[1]);
	}
}

TEST(ValueHelpersKnownVectorsAndRoundTripsForEveryTypeAndOrder)
{
	static const struct {
		mogbus_value_type_t	type;
		double			value;
		size_t			width;
	} cases[] = {
		{ MOGBUS_TYPE_U16, 65535.0, 1 },
		{ MOGBUS_TYPE_U16, 0x1234, 1 },
		{ MOGBUS_TYPE_S16, -2.0, 1 },
		{ MOGBUS_TYPE_U32, 4000000000.0, 2 },
		{ MOGBUS_TYPE_S32, -123456.0, 2 },
		{ MOGBUS_TYPE_F32, 123456.0, 2 },
		{ MOGBUS_TYPE_F32, -0.5, 2 },
		{ MOGBUS_TYPE_U64, 9007199254740992.0, 4 },
		{ MOGBUS_TYPE_S64, -9007199254740992.0, 4 },
		{ MOGBUS_TYPE_F64, 3.141592653589793, 4 },
		{ MOGBUS_TYPE_F64, -1e300, 4 },
	};

	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
	{
		for (size_t w = 0; w < 2; w++)
		{
			for (size_t b = 0; b < 2; b++)
			{
				uint16_t	regs[4] = { 0 };
				size_t		count = 0;
				double		back = 0;

				ASSERT_EQ(mogbus_value_to_registers(cases[c].value, cases[c].type, WORD_ORDERS[w], BYTE_ORDERS[b], regs, &count) == MOGBUS_OK);
				ASSERT_EQ(count == cases[c].width);
				ASSERT_EQ(mogbus_registers_to_value(regs, cases[c].type, WORD_ORDERS[w], BYTE_ORDERS[b], &back) == MOGBUS_OK);
				ASSERT_EQ(back == cases[c].value);
			}
		}
	}

	/* 123456.0f is 0x47F12000; a double carries 64 bits in four registers. */
	{
		uint16_t	regs[4];
		double		v;

		mogbus_value_to_registers(123456.0, MOGBUS_TYPE_F32, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, regs, NULL);
		ASSERT_EQ(regs[0] == 0x47F1 && regs[1] == 0x2000);
		mogbus_value_to_registers(1.0, MOGBUS_TYPE_F64, MOGBUS_WORDS_LOW_FIRST, MOGBUS_BYTES_BIG, regs, NULL);
		ASSERT_EQ(regs[0] == 0x0000 && regs[1] == 0x0000 && regs[2] == 0x0000 && regs[3] == 0x3FF0);
		mogbus_value_to_registers(-2.0, MOGBUS_TYPE_S16, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_LITTLE, regs, NULL);
		ASSERT_EQ(regs[0] == 0xFEFF);
		mogbus_registers_to_value(regs, MOGBUS_TYPE_S16, MOGBUS_WORDS_LOW_FIRST, MOGBUS_BYTES_LITTLE, &v);
		ASSERT_EQ(v == -2.0);
	}
}

TEST(ValueHelpersClampRoundAndKeepNonFiniteFloats)
{
	uint16_t	regs[4];
	double		v;

	mogbus_value_to_registers(70000.0, MOGBUS_TYPE_U16, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, regs, NULL);
	ASSERT_EQ(regs[0] == 0xFFFF);
	mogbus_value_to_registers(2.6, MOGBUS_TYPE_U16, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, regs, NULL);
	ASSERT_EQ(regs[0] == 3);
	mogbus_value_to_registers(NAN, MOGBUS_TYPE_S32, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, regs, NULL);
	ASSERT_EQ(regs[0] == 0 && regs[1] == 0);
	mogbus_value_to_registers(INFINITY, MOGBUS_TYPE_F32, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, regs, NULL);
	mogbus_registers_to_value(regs, MOGBUS_TYPE_F32, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, &v);
	ASSERT_EQ(isinf(v) && v > 0);
	mogbus_value_to_registers(NAN, MOGBUS_TYPE_F64, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, regs, NULL);
	mogbus_registers_to_value(regs, MOGBUS_TYPE_F64, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, &v);
	ASSERT_EQ(isnan(v));
}

TEST(ValueHelpersKeepSixtyFourBitIntegersExact)
{
	static const uint64_t values[] = { 0x0123456789ABCDEFull, 0xFFFFFFFFFFFFFFFFull, 9007199254740993ull };

	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
	{
		for (size_t w = 0; w < 2; w++)
		{
			for (size_t b = 0; b < 2; b++)
			{
				uint16_t	regs[4];
				uint64_t	back = 0;

				ASSERT_EQ(mogbus_u64_to_registers(values[i], WORD_ORDERS[w], BYTE_ORDERS[b], regs) == MOGBUS_OK);
				ASSERT_EQ(mogbus_registers_to_u64(regs, WORD_ORDERS[w], BYTE_ORDERS[b], &back) == MOGBUS_OK);
				ASSERT_EQ(back == values[i]);
			}
		}
	}
	{
		uint16_t	regs[4];

		mogbus_u64_to_registers(0x0123456789ABCDEFull, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, regs);
		ASSERT_EQ(regs[0] == 0x0123 && regs[1] == 0x4567 && regs[2] == 0x89AB && regs[3] == 0xCDEF);
		mogbus_u64_to_registers(0x0123456789ABCDEFull, MOGBUS_WORDS_LOW_FIRST, MOGBUS_BYTES_LITTLE, regs);
		ASSERT_EQ(regs[0] == 0xEFCD && regs[1] == 0xAB89 && regs[2] == 0x6745 && regs[3] == 0x2301);
		mogbus_u64_to_registers((uint64_t)(int64_t)-1, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, regs);
		ASSERT_EQ((int64_t)(((uint64_t)regs[0] << 48) | ((uint64_t)regs[1] << 32) | ((uint64_t)regs[2] << 16) | regs[3]) == -1);
	}
}

TEST(ValueHelpersEncodeAndDecodeText)
{
	uint16_t	regs[4];
	char		text[16];

	ASSERT_EQ(mogbus_text_to_registers("AB", 3, MOGBUS_BYTES_BIG, regs) == MOGBUS_OK);
	ASSERT_EQ(regs[0] == 0x4142 && regs[1] == 0 && regs[2] == 0);
	ASSERT_EQ(mogbus_registers_to_text(regs, 3, MOGBUS_BYTES_BIG, text, sizeof(text)) == MOGBUS_OK);
	ASSERT_EQ(strcmp(text, "AB") == 0);

	ASSERT_EQ(mogbus_text_to_registers("pump1", 3, MOGBUS_BYTES_LITTLE, regs) == MOGBUS_OK);
	ASSERT_EQ(regs[0] == 0x7570 && regs[1] == 0x706D && regs[2] == 0x0031);
	ASSERT_EQ(mogbus_registers_to_text(regs, 3, MOGBUS_BYTES_LITTLE, text, sizeof(text)) == MOGBUS_OK);
	ASSERT_EQ(strcmp(text, "pump1") == 0);

	/* A buffer that is too small gets the start of the text and NOMEM. */
	ASSERT_EQ(mogbus_registers_to_text(regs, 3, MOGBUS_BYTES_LITTLE, text, 4) == MOGBUS_ERR_NOMEM);
	ASSERT_EQ(strcmp(text, "pum") == 0);

	/* Six bytes fill three registers exactly; one more does not fit. */
	ASSERT_EQ(mogbus_text_to_registers("abcdef", 3, MOGBUS_BYTES_BIG, regs) == MOGBUS_OK);
	ASSERT_EQ(mogbus_registers_to_text(regs, 3, MOGBUS_BYTES_BIG, text, sizeof(text)) == MOGBUS_OK);
	ASSERT_EQ(strcmp(text, "abcdef") == 0);
	ASSERT_EQ(mogbus_text_to_registers("abcdefg", 3, MOGBUS_BYTES_BIG, regs) == MOGBUS_ERR_INVALID);
}

TEST(ValueHelpersRejectBadArguments)
{
	uint16_t	regs[4] = { 0 };
	double		v;
	uint64_t	u;
	char		text[8];

	ASSERT_EQ(mogbus_registers_to_value(NULL, MOGBUS_TYPE_U16, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, &v) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_registers_to_value(regs, MOGBUS_TYPE_U16, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, NULL) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_registers_to_value(regs, MOGBUS_TYPE_BOOL, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, &v) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_registers_to_value(regs, MOGBUS_TYPE_STRING, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, &v) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_registers_to_value(regs, (mogbus_value_type_t)99, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, &v) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_registers_to_value(regs, MOGBUS_TYPE_U32, (mogbus_word_order_t)2, MOGBUS_BYTES_BIG, &v) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_registers_to_value(regs, MOGBUS_TYPE_U32, MOGBUS_WORDS_LOW_FIRST, (mogbus_byte_order_t)2, &v) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_value_to_registers(1, MOGBUS_TYPE_U16, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, NULL, NULL) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_value_to_registers(1, MOGBUS_TYPE_BOOL, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, regs, NULL) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_registers_to_u64(NULL, MOGBUS_WORDS_HIGH_FIRST, MOGBUS_BYTES_BIG, &u) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_u64_to_registers(1, (mogbus_word_order_t)7, MOGBUS_BYTES_BIG, regs) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_registers_to_text(regs, 2, MOGBUS_BYTES_BIG, text, 0) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_registers_to_text(regs, 124, MOGBUS_BYTES_BIG, text, sizeof(text)) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_text_to_registers("x", 0, MOGBUS_BYTES_BIG, regs) == MOGBUS_ERR_INVALID);
	ASSERT_EQ(mogbus_text_to_registers(NULL, 1, MOGBUS_BYTES_BIG, regs) == MOGBUS_ERR_INVALID);
}
