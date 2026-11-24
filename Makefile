# modbus-sim build. Modeled on the Makefile vlib/gen_makefile.py generates, but the
# source tree has sub-folders (one per module), so sources are found recursively.

BIN         = modbus-sim
TESTS       = $(BIN)-tests
ASAN_BIN    = modbus-sim-asan
ASAN_TESTS  = modbus-sim-asan-tests

CC          ?= cc
CFLAGS      ?= -std=gnu11 -O2 -g -Werror -Wextra -Wall -Wformat=2 -Wshadow -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
LDFLAGS     ?= -pie -Wl,-z,relro,-z,now
DEPFLAGS    = -MMD -MP
INC         = -I. -Isrc
PKG_CONFIG  ?= pkg-config

# WITH_TLS=auto (default) enables TLS only when OpenSSL development files are
# available. WITH_TLS=1 is an explicit request and fails early otherwise;
# WITH_TLS=0 never invokes pkg-config or passes OpenSSL flags to the compiler.
WITH_TLS ?= auto
ifneq ($(WITH_TLS),0)
OPENSSL_AVAILABLE := $(shell $(PKG_CONFIG) --exists openssl 2>/dev/null && echo 1 || echo 0)
ifeq ($(WITH_TLS),auto)
override WITH_TLS := $(OPENSSL_AVAILABLE)
else ifeq ($(WITH_TLS),1)
ifeq ($(OPENSSL_AVAILABLE),0)
$(error WITH_TLS=1 requires pkg-config and OpenSSL development files ($(PKG_CONFIG) --exists openssl failed))
endif
else
$(error WITH_TLS must be auto, 0 or 1 (got '$(WITH_TLS)'))
endif
endif

TLS_CFLAGS  = -DWITH_TLS=$(WITH_TLS)
LIBS        = -lm
ifeq ($(WITH_TLS),1)
OPENSSL_CFLAGS := $(shell $(PKG_CONFIG) --cflags openssl)
OPENSSL_LIBS   := $(shell $(PKG_CONFIG) --libs openssl)
TLS_CFLAGS     += $(OPENSSL_CFLAGS)
LIBS           += $(OPENSSL_LIBS)
endif

# Keep no-TLS objects separate so switching WITH_TLS without cleaning cannot
# reuse objects compiled with the other TLS setting.
TLS_OBJ_TAG =
ifeq ($(WITH_TLS),0)
TLS_OBJ_TAG = .notls
endif

# SAN=1 builds separate .asan.o objects and links the sanitizer runtimes.
SAN ?= 0

ifeq ($(SAN),1)
OBJEXT      = $(TLS_OBJ_TAG).asan.o
BIN         = $(ASAN_BIN)
TESTS       = $(ASAN_TESTS)
SAN_FLAGS   = -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=undefined,float-cast-overflow -fno-omit-frame-pointer
else
OBJEXT      = $(TLS_OBJ_TAG).o
SAN_FLAGS   =
endif

SRC_DIR     = src
TREE_SRCS   = $(shell find $(SRC_DIR) -name '*.c' ! -name main.c)
MAIN        = $(SRC_DIR)/main.c
ifeq ($(WITH_TLS),1)
SRCS        = $(filter-out $(SRC_DIR)/net/net_tls_stub.c,$(TREE_SRCS))
else
SRCS        = $(filter-out $(SRC_DIR)/net/net_tls.c,$(TREE_SRCS))
endif
OBJS        = $(SRCS:.c=$(OBJEXT)) $(MAIN:.c=$(OBJEXT))

TEST_MAIN   = vlib/vtest.c
ALL_TEST_SRCS = $(wildcard tests/*.c tests/tests_utils/*.c)
ifeq ($(WITH_TLS),1)
TEST_SRCS   = $(ALL_TEST_SRCS)
else
TEST_SRCS   = $(filter-out tests/tests_utils/tls_client.c,$(ALL_TEST_SRCS))
endif
TEST_OBJS   = $(SRCS:.c=$(OBJEXT)) $(TEST_MAIN:.c=$(OBJEXT)) $(TEST_SRCS:.c=$(OBJEXT))

# s7 is third-party code: built with its own relaxed flags, without sanitizers.
# UBSan reports a misaligned load in its local_strncmp (third_party/s7/s7.c:4028).
S7_OBJ      = third_party/s7/s7$(OBJEXT)
S7_FLAGS    = -std=gnu11 -O2 -w -DWITH_SYSTEM_EXTRAS=0 -DWITH_C_LOADER=0

# -MMD writes a .d file next to every object; -MP adds dummy rules so renaming a
# header does not break the build. Including them makes header edits trigger rebuilds.
DEPS        = $(foreach obj,$(OBJS) $(TEST_OBJS) $(S7_OBJ),$(basename $(obj)).d)
# Include every object variant so clean always removes the other configurations.
ALL_SRCS    = $(TREE_SRCS) $(MAIN) $(TEST_MAIN) $(ALL_TEST_SRCS) third_party/s7/s7.c
ALL_OBJS    = $(ALL_SRCS:.c=.o) $(ALL_SRCS:.c=.notls.o) $(ALL_SRCS:.c=.asan.o) $(ALL_SRCS:.c=.notls.asan.o)
ALL_DEPS    = $(ALL_SRCS:.c=.d) $(ALL_SRCS:.c=.notls.d) $(ALL_SRCS:.c=.asan.d) $(ALL_SRCS:.c=.notls.asan.d)

all: $(BIN)

$(BIN): $(OBJS) $(S7_OBJ)
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(LDFLAGS) $^ -o $@ $(LIBS)

tests: $(TESTS) $(BIN)
	./tools/run_tests.sh ./$(TESTS)

$(TESTS): $(TEST_OBJS) $(S7_OBJ)
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(LDFLAGS) $^ -o $@ $(LIBS)

$(S7_OBJ): third_party/s7/s7.c third_party/s7/s7.h
	$(CC) $(S7_FLAGS) $(DEPFLAGS) -c $< -o $@

tests/%$(OBJEXT): tests/%.c
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(TLS_CFLAGS) $(DEPFLAGS) -Wno-unused-function $(INC) -Ivlib -c $< -o $@

# vtest.c relies on a newer gcc default for bool/true/false; force-include it so vlib stays untouched.
vlib/vtest$(OBJEXT): vlib/vtest.c
	$(CC) -std=gnu11 -O2 -g $(SAN_FLAGS) $(TLS_CFLAGS) -include stdbool.h -Wno-unused-function $(DEPFLAGS) -Ivlib -c $< -o $@

%$(OBJEXT): %.c
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(TLS_CFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@

# Leak detection is off: the config tests intentionally keep arenas alive because
# the parsed config points into them. UBSan failures abort (-fno-sanitize-recover).
asan:
	$(MAKE) SAN=1 $(ASAN_BIN) $(ASAN_TESTS)
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 MODBUS_SIM_BIN=./$(ASAN_BIN) ./tools/run_tests.sh ./$(ASAN_TESTS)

clean:
	rm -f $(ALL_OBJS) $(ALL_DEPS) $(BIN) $(TESTS) $(ASAN_BIN) $(ASAN_TESTS)

distclean: clean

re: clean all

commands:
	bear -- $(MAKE)

-include $(DEPS)

.PHONY: all tests asan clean distclean re commands
