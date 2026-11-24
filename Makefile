# modbus-sim build. Modeled on the Makefile vlib/gen_makefile.py generates, but the
# source tree has sub-folders (one per module), so sources are found recursively.

BIN         = modbus-sim
TESTS       = $(BIN)-tests
ASAN_BIN    = modbus-sim-asan
ASAN_TESTS  = modbus-sim-asan-tests

# GNU make predefines CC with origin 'default'. Replace only that built-in value:
# an explicit CC from the environment or command line must still win.
ifeq ($(origin CC),default)
CC          := ccache clang
endif

# BUILD=dev skips optimization and _FORTIFY_SOURCE (which warns without -O) and
# uses separate objects, so switching back and forth never rebuilds blindly.
BUILD       ?= prod

# `dev` selects dev mode for the whole invocation; nested makes (asan, commands)
# must inherit it too. Without this, the parent make would rebuild the forwarded
# goals in prod mode after the sub-make.
ifneq ($(filter dev,$(MAKECMDGOALS)),)
override BUILD := dev
endif
export BUILD

ifeq ($(BUILD),dev)
DEV_OBJ_TAG   = .dev
OPT_FLAGS     = -O0 -g
FORTIFY_FLAGS =
else ifeq ($(BUILD),prod)
DEV_OBJ_TAG   =
OPT_FLAGS     = -O2 -g
FORTIFY_FLAGS = -D_FORTIFY_SOURCE=2
else
$(error BUILD must be dev or prod (got '$(BUILD)'))
endif

CFLAGS      ?= -std=gnu11 $(OPT_FLAGS) -Werror -Wextra -Wall -Wformat=2 -Wshadow $(FORTIFY_FLAGS) -fstack-protector-strong -fPIE
LDFLAGS     ?= -pie -Wl,-z,relro,-z,now -fuse-ld=mold
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

# SAN=1 and BUILD=dev build separate objects and link their runtimes/flags.
SAN ?= 0

ifeq ($(SAN),1)
OBJEXT      = $(DEV_OBJ_TAG)$(TLS_OBJ_TAG).asan.o
BIN         = $(ASAN_BIN)
TESTS       = $(ASAN_TESTS)
SAN_FLAGS   = -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=undefined,float-cast-overflow -fno-omit-frame-pointer
else
OBJEXT      = $(DEV_OBJ_TAG)$(TLS_OBJ_TAG).o
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
# It only parses config at startup, so -O0 avoids paying most of the build cost.
S7_OBJ      = third_party/s7/s7$(OBJEXT)
S7_FLAGS    = -std=gnu11 -O0 -w -DWITH_SYSTEM_EXTRAS=0 -DWITH_C_LOADER=0

# Relink when switching BUILD even though the binary name stays the same.
BUILD_STAMP = .build-mode

# -MMD writes a .d file next to every object; -MP adds dummy rules so renaming a
# header does not break the build. Including them makes header edits trigger rebuilds.
DEPS        = $(foreach obj,$(OBJS) $(TEST_OBJS) $(S7_OBJ),$(basename $(obj)).d)
# Include every object variant so clean always removes the other configurations.
ALL_SRCS    = $(TREE_SRCS) $(MAIN) $(TEST_MAIN) $(ALL_TEST_SRCS) third_party/s7/s7.c
ALL_OBJ_SUFFIXES = .o .notls.o .asan.o .notls.asan.o .dev.o .dev.notls.o .dev.asan.o .dev.notls.asan.o
ALL_OBJS    = $(foreach suffix,$(ALL_OBJ_SUFFIXES),$(ALL_SRCS:.c=$(suffix)))
ALL_DEPS    = $(ALL_OBJS:.o=.d)

all: $(BIN)

# Fast local build: make -j$(nproc) dev [tests|clean|...]. With no other goal,
# `dev` builds the default binary; with extra goals it is just a mode switch.
ifeq ($(filter-out dev,$(MAKECMDGOALS)),)
dev: all
else
dev:
	@:
endif

$(BIN): $(BUILD_STAMP) $(OBJS) $(S7_OBJ)
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(LDFLAGS) $(filter-out $(BUILD_STAMP),$^) -o $@ $(LIBS)

tests: $(TESTS) $(BIN)
	./tools/run_tests.sh ./$(TESTS)

$(TESTS): $(BUILD_STAMP) $(TEST_OBJS) $(S7_OBJ)
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(LDFLAGS) $(filter-out $(BUILD_STAMP),$^) -o $@ $(LIBS)

$(S7_OBJ): third_party/s7/s7.c third_party/s7/s7.h
	$(CC) $(S7_FLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_STAMP): FORCE
	@printf '%s\n' '$(BUILD)' | cmp -s - $@ 2>/dev/null || printf '%s\n' '$(BUILD)' > $@

tests/%$(OBJEXT): tests/%.c
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(TLS_CFLAGS) $(DEPFLAGS) -Wno-unused-function $(INC) -Ivlib -c $< -o $@

# vtest.c relies on a newer gcc default for bool/true/false; force-include it so vlib stays untouched.
vlib/vtest$(OBJEXT): vlib/vtest.c
	$(CC) -std=gnu11 $(OPT_FLAGS) $(SAN_FLAGS) $(TLS_CFLAGS) -include stdbool.h -Wno-unused-function $(DEPFLAGS) -Ivlib -c $< -o $@

%$(OBJEXT): %.c
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(TLS_CFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@

# Leak detection is off: the config tests intentionally keep arenas alive because
# the parsed config points into them. UBSan failures abort (-fno-sanitize-recover).
asan:
	$(MAKE) SAN=1 $(ASAN_BIN) $(ASAN_TESTS)
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 MODBUS_SIM_BIN=./$(ASAN_BIN) ./tools/run_tests.sh ./$(ASAN_TESTS)

clean:
	rm -f $(ALL_OBJS) $(ALL_DEPS) $(BUILD_STAMP) $(BIN) $(TESTS) $(ASAN_BIN) $(ASAN_TESTS)

distclean: clean

re: clean all

commands:
	bear -- $(MAKE)

-include $(DEPS)

.PHONY: all dev tests asan clean distclean re commands FORCE
