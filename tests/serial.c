#include "vtest.h"
#include "util/serial.h"

TEST(SerialLineFlagsDefaultIs8N1)
{
	tcflag_t clear = 0;
	tcflag_t set = 0;

	ASSERT_EQ(serial_line_flags(8, PARITY_NONE, 1, &clear, &set));
	ASSERT_EQ((clear & (CSIZE | PARENB | PARODD | CSTOPB)) == (CSIZE | PARENB | PARODD | CSTOPB));
#ifdef CMSPAR
	ASSERT_EQ((clear & CMSPAR) != 0);
#endif
	ASSERT_EQ(set == CS8);
}

TEST(SerialLineFlagsSevenDataBitsEven)
{
	tcflag_t clear = 0;
	tcflag_t set = 0;

	ASSERT_EQ(serial_line_flags(7, PARITY_EVEN, 1, &clear, &set));
	ASSERT_EQ((clear & CSIZE) == CSIZE);
	ASSERT_EQ(set == (CS7 | PARENB));
	ASSERT_EQ((set & CSTOPB) == 0);
}

TEST(SerialLineFlagsParityChoices)
{
	tcflag_t clear = 0;
	tcflag_t set = 0;

	ASSERT_EQ(serial_line_flags(8, PARITY_EVEN, 1, &clear, &set));
	ASSERT_EQ(set == (CS8 | PARENB));

	ASSERT_EQ(serial_line_flags(8, PARITY_ODD, 1, &clear, &set));
	ASSERT_EQ(set == (CS8 | PARENB | PARODD));

#ifdef CMSPAR
	ASSERT_EQ(serial_line_flags(8, PARITY_MARK, 1, &clear, &set));
	ASSERT_EQ(set == (CS8 | PARENB | PARODD | CMSPAR));

	ASSERT_EQ(serial_line_flags(8, PARITY_SPACE, 1, &clear, &set));
	ASSERT_EQ(set == (CS8 | PARENB | CMSPAR));
#else
	ASSERT_EQ(!serial_line_flags(8, PARITY_MARK, 1, &clear, &set));
	ASSERT_EQ(!serial_line_flags(8, PARITY_SPACE, 1, &clear, &set));
#endif
}

TEST(SerialLineFlagsTwoStopBits)
{
	tcflag_t clear = 0;
	tcflag_t set = 0;

	ASSERT_EQ(serial_line_flags(8, PARITY_NONE, 2, &clear, &set));
	ASSERT_EQ(set == (CS8 | CSTOPB));

	ASSERT_EQ(serial_line_flags(8, PARITY_EVEN, 2, &clear, &set));
	ASSERT_EQ(set == (CS8 | PARENB | CSTOPB));
}
