# mogbus build. Modeled on the Makefile vlib/gen_makefile.py generates, but the
# source tree has sub-folders (one per module), so sources are found recursively.

BIN         = mogbus
TESTS       = $(BIN)-tests
ASAN_BIN    = mogbus-asan
ASAN_TESTS  = mogbus-asan-tests
LIB_STATIC  = libmogbus.a
LIB_SHARED  = libmogbus.so
EXAMPLE_SRC = examples/embed_tcp.c
EXAMPLE_SO  = examples/embed_tcp_shared
EXAMPLE_A   = examples/embed_tcp_static

# Prefer clang, fall back to gcc then cc. ccache and mold are independent
# speed-ups, each used only when installed.
# GNU make predefines CC with origin 'default'. Replace only that built-in value:
# an explicit CC from the environment or command line must still win.
ifeq ($(origin CC),default)
DEFAULT_CC  := $(shell if command -v clang >/dev/null 2>&1; then echo clang; \
                       elif command -v gcc >/dev/null 2>&1; then echo gcc; \
                       else echo cc; fi)
CCACHE      := $(shell command -v ccache >/dev/null 2>&1 && echo ccache)
CC          := $(if $(CCACHE),$(CCACHE) $(DEFAULT_CC),$(DEFAULT_CC))
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

CFLAGS      ?= -std=gnu11 $(OPT_FLAGS) -Werror -Wextra -Wall -Wformat=2 -Wshadow $(FORTIFY_FLAGS) -fstack-protector-strong -fPIC -fvisibility=hidden
# -fPIC (not -fPIE) so the same objects serve the executables, libmogbus.a and
# libmogbus.so; PIC code is valid in a PIE and costs next to nothing on x86-64.
# -fvisibility=hidden leaves only the MOGBUS_API symbols exported from the .so.
# Hardening applies to every link, even when LDFLAGS is overridden; skip flags
# the user already supplied so overrides stay free of duplicates.
HARDEN_LDFLAGS = -pie -Wl,-z,relro,-z,now
# Probe the chosen compiler (including a ccache prefix) before adding mold; old
# gcc versions reject -fuse-ld=mold. An explicit LDFLAGS skips the probe entirely.
ifeq ($(origin LDFLAGS),undefined)
MOLD_LDFLAGS := $(shell if command -v mold >/dev/null 2>&1 && \
                            printf 'int main(void){return 0;}\n' | $(CC) -x c - -fuse-ld=mold -o /dev/null >/dev/null 2>&1; then \
                        echo -fuse-ld=mold; \
                 fi)
LDFLAGS     ?= $(MOLD_LDFLAGS)
endif
LINK_FLAGS   = $(LDFLAGS) $(filter-out $(LDFLAGS),$(HARDEN_LDFLAGS))
DEPFLAGS    = -MMD -MP
INC         = -I. -Isrc -Iinclude
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

# WITH_CONFIG=0 leaves out the Scheme config loader and vendored s7: the libraries
# then serve only devices built in code, and there is no mogbus binary (it needs a
# config), so `all` builds the libraries instead.
WITH_CONFIG ?= 1
ifneq ($(filter-out 0 1,$(WITH_CONFIG)),)
$(error WITH_CONFIG must be 0 or 1 (got '$(WITH_CONFIG)'))
endif

TLS_CFLAGS  = -DWITH_TLS=$(WITH_TLS) -DWITH_CONFIG=$(WITH_CONFIG)
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

CONFIG_OBJ_TAG =
ifeq ($(WITH_CONFIG),0)
CONFIG_OBJ_TAG = .nocfg
endif

# SAN=1 and BUILD=dev build separate objects and link their runtimes/flags.
SAN ?= 0

ifeq ($(SAN),1)
OBJEXT      = $(DEV_OBJ_TAG)$(TLS_OBJ_TAG)$(CONFIG_OBJ_TAG).asan.o
BIN         = $(ASAN_BIN)
TESTS       = $(ASAN_TESTS)
SAN_FLAGS   = -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=undefined,float-cast-overflow -fno-omit-frame-pointer
else
OBJEXT      = $(DEV_OBJ_TAG)$(TLS_OBJ_TAG)$(CONFIG_OBJ_TAG).o
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
MAIN_OBJ    = $(MAIN:.c=$(OBJEXT))
OBJS        = $(SRCS:.c=$(OBJEXT)) $(MAIN_OBJ)

# s7 is third-party code: built with its own relaxed flags, without sanitizers.
# UBSan reports a misaligned load in its local_strncmp (third_party/s7/s7.c:4028).
# It only parses config at startup, so -O0 avoids paying most of the build cost.
S7_OBJ      = third_party/s7/s7$(OBJEXT)
S7_FLAGS    = -std=gnu11 -O0 -w -fstack-protector-strong -fPIC -fvisibility=hidden -DWITH_SYSTEM_EXTRAS=0 -DWITH_C_LOADER=0

# Without config there is no binary, no s7 and nothing that loads a config file.
ifeq ($(WITH_CONFIG),0)
SRCS        := $(filter-out $(SRC_DIR)/config/% $(SRC_DIR)/server/server_config.c,$(SRCS))
CFG_S7_OBJ  =
else
CFG_S7_OBJ  = $(S7_OBJ)
endif

TEST_MAIN   = vlib/vtest.c
ALL_TEST_SRCS = $(wildcard tests/*.c tests/tests_utils/*.c)
TEST_SRCS   = $(ALL_TEST_SRCS)
ifneq ($(WITH_TLS),1)
TEST_SRCS   := $(filter-out tests/tests_utils/tls_client.c,$(TEST_SRCS))
endif
# These tests start the real binary or call config_load; the others run unchanged.
ifeq ($(WITH_CONFIG),0)
TEST_SRCS   := $(filter-out tests/config.c tests/ascii_cli.c tests/rtu_cli.c tests/tcp_server.c \
                            tests/machine_e2e.c tests/ascii_tcp_server.c tests/rtu_tcp_server.c \
                            tests/udp_server.c tests/%_udp_server.c,$(TEST_SRCS))
endif
# vtest.c defines the arena implementation itself, so the project's is left out.
TEST_OBJS   = $(filter-out $(SRC_DIR)/util/arena_impl$(OBJEXT),$(SRCS:.c=$(OBJEXT))) $(TEST_MAIN:.c=$(OBJEXT)) $(TEST_SRCS:.c=$(OBJEXT))

# Everything but main.c, plus s7 so the library is self-contained.
LIB_OBJS    = $(SRCS:.c=$(OBJEXT)) $(CFG_S7_OBJ)

# The binary is main.o linked against a static library of the same build variant
# (libmogbus.dev.a, libmogbus.notls.asan.a, ...). The prod variant is LIB_STATIC itself.
LIB_VARIANT = libmogbus$(OBJEXT:.o=.a)

# Relink when switching BUILD, WITH_TLS or WITH_CONFIG even though the output
# names stay the same (objects of every variant may already be up to date).
BUILD_STAMP = .build-mode

# -MMD writes a .d file next to every object; -MP adds dummy rules so renaming a
# header does not break the build. Including them makes header edits trigger rebuilds.
DEPS        = $(foreach obj,$(OBJS) $(TEST_OBJS) $(S7_OBJ),$(basename $(obj)).d)
# Include every object variant so clean always removes the other configurations.
ALL_SRCS    = $(TREE_SRCS) $(MAIN) $(TEST_MAIN) $(ALL_TEST_SRCS) third_party/s7/s7.c
ALL_OBJ_SUFFIXES = $(foreach d,.- .dev,$(foreach t,.- .notls,$(foreach c,.- .nocfg,$(foreach a,.o .asan.o,$(subst .-,,$d)$(subst .-,,$t)$(subst .-,,$c)$a))))
ALL_OBJS    = $(foreach suffix,$(ALL_OBJ_SUFFIXES),$(ALL_SRCS:.c=$(suffix)))
ALL_DEPS    = $(ALL_OBJS:.o=.d)
ALL_LIBS    = $(foreach suffix,$(ALL_OBJ_SUFFIXES),libmogbus$(suffix:.o=.a))

# `all` builds only the binary; `make lib` adds libmogbus.a and libmogbus.so.
# Without config there is no binary, so `all` builds the libraries.
ifeq ($(WITH_CONFIG),1)
PROGRAMS    = $(BIN)
all: $(BIN)
else
PROGRAMS    =
all: lib
endif

lib: $(LIB_STATIC) $(LIB_SHARED)

# Fast local build: make -j$(nproc) dev [tests|clean|...]. With no other goal,
# `dev` builds the default binary; with extra goals it is just a mode switch.
ifeq ($(filter-out dev,$(MAKECMDGOALS)),)
dev: all
else
dev:
	@:
endif

ifeq ($(WITH_CONFIG),1)
$(BIN): $(BUILD_STAMP) $(MAIN_OBJ) $(LIB_VARIANT)
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(filter-out $(BUILD_STAMP),$^) -o $@ $(LIBS)
endif

tests: $(TESTS) $(PROGRAMS) lib-check
ifeq ($(WITH_CONFIG),1)
	./tools/check_toolchain.sh
endif
	./tools/run_tests.sh ./$(TESTS)

# ar only adds members, so start from scratch to drop objects of another build mode.
$(LIB_VARIANT): $(BUILD_STAMP) $(LIB_OBJS)
	$(RM) $@
	$(AR) rcs $@ $(filter-out $(BUILD_STAMP),$^)

# The user-facing archive is the current variant (the same file in a prod build).
ifneq ($(LIB_VARIANT),$(LIB_STATIC))
$(LIB_STATIC): $(BUILD_STAMP) $(LIB_VARIANT)
	cp $(LIB_VARIANT) $@
endif

# -pie is for executables only; relro/now stay.
$(LIB_SHARED): $(BUILD_STAMP) $(LIB_OBJS)
	$(CC) $(CFLAGS) -shared -Wl,-soname,$@ $(filter-out -pie,$(LDFLAGS)) -Wl,-z,relro,-z,now $(filter-out $(BUILD_STAMP),$^) -o $@ $(LIBS)

# The example sees only include/mogbus.h, which proves the public header and the
# exported symbols are enough. The .so is found through an $ORIGIN rpath.
$(EXAMPLE_SO): $(EXAMPLE_SRC) include/mogbus.h $(LIB_SHARED)
	$(CC) $(CFLAGS) -Iinclude $< ./$(LIB_SHARED) $(LINK_FLAGS) -Wl,-rpath,'$$ORIGIN/..' -o $@

$(EXAMPLE_A): $(EXAMPLE_SRC) include/mogbus.h $(LIB_STATIC)
	$(CC) $(CFLAGS) -Iinclude $< $(LIB_STATIC) $(LINK_FLAGS) -o $@ $(LIBS)

# Builds the libraries, requires the .so to export only mogbus_* symbols, and runs
# the example against both.
lib-check: $(EXAMPLE_SO) $(EXAMPLE_A)
	@bad=$$(nm -D --defined-only $(LIB_SHARED) | awk '{print $$3}' | \
		grep -vE '^(mogbus_|_init$$|_fini$$|_edata$$|_end$$|__bss_start$$)' || true); \
	if [ -n "$$bad" ]; then echo "$(LIB_SHARED) exports non-API symbols:" $$bad >&2; exit 1; fi; \
	nm -D --defined-only $(LIB_SHARED) | grep -q ' mogbus_create$$' || { echo "$(LIB_SHARED) lacks mogbus_create" >&2; exit 1; }
	./$(EXAMPLE_SO)
	./$(EXAMPLE_A)

$(TESTS): $(BUILD_STAMP) $(TEST_OBJS) $(CFG_S7_OBJ)
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(filter-out $(BUILD_STAMP),$^) -o $@ $(LIBS)

$(S7_OBJ): third_party/s7/s7.c third_party/s7/s7.h
	$(CC) $(S7_FLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_STAMP): FORCE
	@printf '%s\n' '$(BUILD) tls=$(WITH_TLS) config=$(WITH_CONFIG)' | cmp -s - $@ 2>/dev/null || \
		printf '%s\n' '$(BUILD) tls=$(WITH_TLS) config=$(WITH_CONFIG)' > $@

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
	$(MAKE) SAN=1 $(if $(PROGRAMS),$(ASAN_BIN)) $(ASAN_TESTS)
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 MOGBUS_BIN=./$(ASAN_BIN) ./tools/run_tests.sh ./$(ASAN_TESTS)

clean:
	rm -f $(ALL_OBJS) $(ALL_DEPS) $(ALL_LIBS) $(BUILD_STAMP) $(BIN) $(TESTS) $(ASAN_BIN) $(ASAN_TESTS) \
		$(LIB_STATIC) $(LIB_SHARED) $(EXAMPLE_SO) $(EXAMPLE_A)

distclean: clean

re: clean all

commands:
	bear -- $(MAKE)

-include $(DEPS)

.PHONY: all dev lib lib-check tests asan clean distclean re commands FORCE
