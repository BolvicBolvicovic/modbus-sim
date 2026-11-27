/** @file
 *  @brief vtest ASSERT that names the current table case when it fails.
 *  Include after vtest.h, set test_case before each case and reset it to 0 after the loop.
 */
#ifndef TESTS_TESTS_UTILS_TEST_CASE_H
#define TESTS_TESTS_UTILS_TEST_CASE_H

#include "vtest.h"
#include <setjmp.h>
#include <stdio.h>

static const char* test_case;

#undef ASSERT
#define ASSERT(eq)                                                      \
	do                                                              \
	{                                                               \
		if (!(eq))                                              \
		{                                                       \
			if (test_case)                                  \
				fprintf(stderr, "case: %s\n", test_case);\
			g_assert_ctx.line      = __LINE__;              \
			g_assert_ctx.assertion = #eq;                   \
			longjmp(g_assert_ctx.jmpbuffer, 1);             \
		}                                                       \
	} while (0)

#endif
