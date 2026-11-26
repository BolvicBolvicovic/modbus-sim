#include "vtest.h"
#include "os/os_serial.h"
#include <termios.h>

TEST(SerialLineFlagsDefaultIs8N1)
{
	u32 clear = 0;
	u32 set = 0;

	ASSERT_EQ(os_serial_line_flags(8, PARITY_NONE, 1, &clear, &set));
	ASSERT_EQ((clear & (CSIZE | PARENB | PARODD | CSTOPB)) == (CSIZE | PARENB | PARODD | CSTOPB));
#ifdef CMSPAR
	ASSERT_EQ((clear & CMSPAR) != 0);
#endif
	ASSERT_EQ(set == CS8);
}

TEST(SerialLineFlagsSevenDataBitsEven)
{
	u32 clear = 0;
	u32 set = 0;

	ASSERT_EQ(os_serial_line_flags(7, PARITY_EVEN, 1, &clear, &set));
	ASSERT_EQ((clear & CSIZE) == CSIZE);
	ASSERT_EQ(set == (CS7 | PARENB));
	ASSERT_EQ((set & CSTOPB) == 0);
}

TEST(SerialLineFlagsParityChoices)
{
	u32 clear = 0;
	u32 set = 0;

	ASSERT_EQ(os_serial_line_flags(8, PARITY_EVEN, 1, &clear, &set));
	ASSERT_EQ(set == (CS8 | PARENB));

	ASSERT_EQ(os_serial_line_flags(8, PARITY_ODD, 1, &clear, &set));
	ASSERT_EQ(set == (CS8 | PARENB | PARODD));

#ifdef CMSPAR
	ASSERT_EQ(os_serial_line_flags(8, PARITY_MARK, 1, &clear, &set));
	ASSERT_EQ(set == (CS8 | PARENB | PARODD | CMSPAR));

	ASSERT_EQ(os_serial_line_flags(8, PARITY_SPACE, 1, &clear, &set));
	ASSERT_EQ(set == (CS8 | PARENB | CMSPAR));
#else
	ASSERT_EQ(!os_serial_line_flags(8, PARITY_MARK, 1, &clear, &set));
	ASSERT_EQ(!os_serial_line_flags(8, PARITY_SPACE, 1, &clear, &set));
#endif
}

TEST(SerialLineFlagsTwoStopBits)
{
	u32 clear = 0;
	u32 set = 0;

	ASSERT_EQ(os_serial_line_flags(8, PARITY_NONE, 2, &clear, &set));
	ASSERT_EQ(set == (CS8 | CSTOPB));

	ASSERT_EQ(os_serial_line_flags(8, PARITY_EVEN, 2, &clear, &set));
	ASSERT_EQ(set == (CS8 | PARENB | CSTOPB));
}
