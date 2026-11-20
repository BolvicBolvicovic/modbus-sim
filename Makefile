# modbus-sim build. Modeled on the Makefile vlib/gen_makefile.py generates, but the
# source tree has sub-folders (one per module), so sources are found recursively.

BIN         = modbus-sim
TESTS       = $(BIN)-tests
ASAN_BIN    = modbus-sim-asan
ASAN_TESTS  = modbus-sim-asan-tests

CC          = gcc
CFLAGS      = -std=gnu11 -O2 -g -Werror -Wextra -Wall
DEPFLAGS    = -MMD -MP
INC         = -I. -Isrc
LIBS        = -lm

# SAN=1 builds separate .asan.o objects and links the sanitizer runtimes.
SAN ?= 0

ifeq ($(SAN),1)
OBJEXT      = .asan.o
BIN         = $(ASAN_BIN)
TESTS       = $(ASAN_TESTS)
SAN_FLAGS   = -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
else
OBJEXT      = .o
SAN_FLAGS   =
endif

SRC_DIR     = src
SRCS        = $(shell find $(SRC_DIR) -name '*.c' ! -name main.c)
MAIN        = $(SRC_DIR)/main.c
OBJS        = $(SRCS:.c=$(OBJEXT)) $(MAIN:.c=$(OBJEXT))

TEST_MAIN   = vlib/vtest.c
TEST_SRCS   = $(wildcard tests/*.c)
TEST_OBJS   = $(SRCS:.c=$(OBJEXT)) $(TEST_MAIN:.c=$(OBJEXT)) $(TEST_SRCS:.c=$(OBJEXT))

# s7 is third-party code: built with its own relaxed flags, without sanitizers.
# UBSan reports a misaligned load in its local_strncmp (third_party/s7/s7.c:4028).
S7_OBJ      = third_party/s7/s7$(OBJEXT)
S7_FLAGS    = -std=gnu11 -O2 -w -DWITH_SYSTEM_EXTRAS=0 -DWITH_C_LOADER=0

# -MMD writes a .d file next to every object; -MP adds dummy rules so renaming a
# header does not break the build. Including them makes header edits trigger rebuilds.
DEPS        = $(foreach obj,$(OBJS) $(TEST_OBJS) $(S7_OBJ),$(basename $(obj)).d)
ALL_SRCS    = $(SRCS) $(MAIN) $(TEST_MAIN) $(TEST_SRCS) third_party/s7/s7.c
ALL_OBJS    = $(ALL_SRCS:.c=.o) $(ALL_SRCS:.c=.asan.o)
ALL_DEPS    = $(ALL_SRCS:.c=.d) $(ALL_SRCS:.c=.asan.d)

all: $(BIN)

$(BIN): $(OBJS) $(S7_OBJ)
	$(CC) $(CFLAGS) $(SAN_FLAGS) $^ -o $@ $(LIBS)

tests: $(TESTS)
	./tools/run_tests.sh ./$(TESTS)

$(TESTS): $(TEST_OBJS) $(S7_OBJ)
	$(CC) $(CFLAGS) $(SAN_FLAGS) $^ -o $@ $(LIBS)

$(S7_OBJ): third_party/s7/s7.c third_party/s7/s7.h
	$(CC) $(S7_FLAGS) $(DEPFLAGS) -c $< -o $@

tests/%$(OBJEXT): tests/%.c
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(DEPFLAGS) -Wno-unused-function $(INC) -Ivlib -c $< -o $@

# vtest.c relies on a newer gcc default for bool/true/false; force-include it so vlib stays untouched.
vlib/vtest$(OBJEXT): vlib/vtest.c
	$(CC) -std=gnu11 -O2 -g $(SAN_FLAGS) -include stdbool.h -Wno-unused-function $(DEPFLAGS) -Ivlib -c $< -o $@

%$(OBJEXT): %.c
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(DEPFLAGS) $(INC) -c $< -o $@

# Leak detection is off: the config tests intentionally keep arenas alive because
# the parsed config points into them. UBSan failures abort (-fno-sanitize-recover).
asan:
	$(MAKE) SAN=1 $(ASAN_TESTS)
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 ./tools/run_tests.sh ./$(ASAN_TESTS)

clean:
	rm -f $(ALL_OBJS) $(ALL_DEPS) $(BIN) $(TESTS) $(ASAN_BIN) $(ASAN_TESTS)

distclean: clean

re: clean all

commands:
	bear -- $(MAKE)

-include $(DEPS)

.PHONY: all tests asan clean distclean re commands
