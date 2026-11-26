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
# Reproducible output: strip the build directory from debug info and __FILE__
# (compile commands run from $(CURDIR) with relative paths). Nothing may embed
# __DATE__/__TIME__ or the clock; archives are written deterministically (rcsD).
REPRO_FLAGS = -ffile-prefix-map=$(CURDIR)=.
# Source revision shown by `mogbus --version`; only main.c sees it, only when
# building from a git checkout. GIT_REVISION= (empty) disables it.
GIT_REVISION ?= $(shell git describe --always --dirty 2>/dev/null)
REV_STAMP   = .revision
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
TREE_SRCS   = $(shell find $(SRC_DIR) -name '*.c' ! -name main.c | LC_ALL=C sort)
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
S7_FLAGS    = -std=gnu11 -O0 -w -fstack-protector-strong -fPIC -fvisibility=hidden -DWITH_SYSTEM_EXTRAS=0 -DWITH_C_LOADER=0 $(REPRO_FLAGS)

# Without config there is no binary, no s7 and nothing that loads a config file.
ifeq ($(WITH_CONFIG),0)
SRCS        := $(filter-out $(SRC_DIR)/config/% $(SRC_DIR)/server/server_config.c,$(SRCS))
CFG_S7_OBJ  =
else
CFG_S7_OBJ  = $(S7_OBJ)
endif

TEST_MAIN   = vlib/vtest.c
ALL_TEST_SRCS = $(sort $(wildcard tests/*.c tests/tests_utils/*.c))
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

# Python package (python/): `python` copies the shared library next to the sources so that
# `pip install ./python` ships it; `python-tests` runs the unittest suite against it.
python: $(LIB_SHARED)
	cp $(LIB_SHARED) python/mogbus/libmogbus.so

# layout.c prints the C struct layouts that the ctypes classes must match (test_layout).
python-tests: $(LIB_SHARED)
	mkdir -p build
	$(CC) -Iinclude python/tests/layout.c -o build/py_layout
	build/py_layout > build/py_layout.txt
	MOGBUS_LIB=$(CURDIR)/$(LIB_SHARED) MOGBUS_LAYOUT=$(CURDIR)/build/py_layout.txt PYTHONPATH=python python3 -m unittest discover -s python/tests

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
	$(AR) rcsD $@ $(filter-out $(BUILD_STAMP),$^)

# The user-facing archive is the current variant (the same file in a prod build).
ifneq ($(LIB_VARIANT),$(LIB_STATIC))
$(LIB_STATIC): $(BUILD_STAMP) $(LIB_VARIANT)
	cp $(LIB_VARIANT) $@
endif

# -pie is for executables only; relro/now stay.
$(LIB_SHARED): $(BUILD_STAMP) $(LIB_OBJS)
	$(CC) $(CFLAGS) $(REPRO_FLAGS) -shared -Wl,-soname,$@ $(filter-out -pie,$(LDFLAGS)) -Wl,-z,relro,-z,now $(filter-out $(BUILD_STAMP),$^) -o $@ $(LIBS)

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

$(MAIN_OBJ): $(REV_STAMP)
$(MAIN_OBJ): REV_FLAGS = $(if $(GIT_REVISION),-DMOGBUS_GIT_REVISION='"$(GIT_REVISION)"')

$(REV_STAMP): FORCE
	@printf '%s\n' '$(GIT_REVISION)' | cmp -s - $@ 2>/dev/null || printf '%s\n' '$(GIT_REVISION)' > $@

tests/%$(OBJEXT): tests/%.c
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(TLS_CFLAGS) $(DEPFLAGS) -Wno-unused-function $(INC) -Ivlib -c $< -o $@

# vtest.c relies on a newer gcc default for bool/true/false; force-include it so vlib stays untouched.
vlib/vtest$(OBJEXT): vlib/vtest.c
	$(CC) -std=gnu11 $(OPT_FLAGS) $(SAN_FLAGS) $(TLS_CFLAGS) -include stdbool.h -Wno-unused-function $(DEPFLAGS) -Ivlib -c $< -o $@

%$(OBJEXT): %.c
	$(CC) $(CFLAGS) $(REPRO_FLAGS) $(REV_FLAGS) $(SAN_FLAGS) $(TLS_CFLAGS) $(DEPFLAGS) $(INC) -c $< -o $@

# Leak detection is off: the config tests intentionally keep arenas alive because
# the parsed config points into them. UBSan failures abort (-fno-sanitize-recover).
asan:
	$(MAKE) SAN=1 $(if $(PROGRAMS),$(ASAN_BIN)) $(ASAN_TESTS)
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 MOGBUS_BIN=./$(ASAN_BIN) ./tools/run_tests.sh ./$(ASAN_TESTS)

# Release tarball: the prod build plus headers, docs and profiles, packed with sorted
# names, owner 0, a fixed mtime and gzip -n so the same commit gives the same bytes.
# SOURCE_DATE_EPOCH (default: the commit time) fixes mtimes and the SBOM timestamp.
VERSION     = $(shell awk '/^\#define MOGBUS_VERSION_(MAJOR|MINOR|PATCH)/ { v = v (v ? "." : "") $$3 } END { print v }' include/mogbus.h)
ARCH        = $(shell uname -m)
DIST_NAME   = mogbus-$(VERSION)-linux-$(ARCH)
DIST_DIR    = dist
dist:
ifneq ($(BUILD)$(SAN)$(WITH_CONFIG),prod01)
	$(error make dist needs BUILD=prod, SAN=0 and WITH_CONFIG=1)
endif
	@$(MAKE) $(BIN) lib
	@epoch=$${SOURCE_DATE_EPOCH:-$$(git log -1 --format=%ct 2>/dev/null)}; \
	[ -n "$$epoch" ] || { echo "make dist: set SOURCE_DATE_EPOCH (no git history)" >&2; exit 1; }; \
	stage=$(DIST_DIR)/$(DIST_NAME); \
	$(RM) -r $(DIST_DIR) && mkdir -p $$stage/bin $$stage/lib $$stage/include \
		$$stage/share/doc/mogbus/docs $$stage/share/mogbus/profiles && \
	cp $(BIN) $$stage/bin/ && cp $(LIB_STATIC) $(LIB_SHARED) $$stage/lib/ && \
	cp include/mogbus.h $$stage/include/ && \
	cp README.md LICENSE NOTICE.md $$stage/share/doc/mogbus/ && \
	cp docs/user/*.md $$stage/share/doc/mogbus/docs/ && cp profiles/*.scm $$stage/share/mogbus/profiles/ && \
	find $$stage -type d -exec chmod 755 {} + && find $$stage -type f -exec chmod 644 {} + && \
	chmod 755 $$stage/bin/$(BIN) $$stage/lib/$(LIB_SHARED) && \
	tar -C $(DIST_DIR) --sort=name --mtime=@$$epoch --owner=0 --group=0 --numeric-owner \
		--format=gnu -cf - $(DIST_NAME) | gzip -n -9 > $(DIST_DIR)/$(DIST_NAME).tar.gz && \
	SOURCE_DATE_EPOCH=$$epoch ./tools/gen_sbom.sh $(DIST_DIR)/$(DIST_NAME).tar.gz $$stage/bin/$(BIN) \
		$(VERSION) $(WITH_TLS) $(DIST_DIR)/$(DIST_NAME).spdx.json && \
	$(RM) -r $$stage && \
	(cd $(DIST_DIR) && sha256sum $(DIST_NAME).tar.gz $(DIST_NAME).spdx.json > SHA256SUMS) && \
	cat $(DIST_DIR)/SHA256SUMS

# libFuzzer harnesses (tests/fuzz/*.c) with ASan + UBSan, built straight from the parser and device
# sources (no TLS, no config loader, no s7). `make fuzz-run FUZZ_SECONDS=N` runs each for N s.
# tests/fuzz/ is a subdirectory on purpose: the tests wildcard (tests/*.c) does not recurse, so the
# harnesses (each defines LLVMFuzzerTestOneInput) are never linked into the vtest binary.
FUZZ_CC      ?= clang
FUZZ_DIR      = build/fuzz
FUZZ_NAMES    = $(patsubst tests/fuzz/fuzz_%.c,%,$(sort $(wildcard tests/fuzz/fuzz_*.c)))
FUZZ_BINS     = $(addprefix $(FUZZ_DIR)/fuzz_,$(FUZZ_NAMES))
FUZZ_SRCS     = $(sort $(wildcard src/modbus/*.c src/device/*.c src/util/*.c)) \
		src/net/net_common.c src/net/net_control.c src/net/net_obs.c src/net/rtu_framer.c
FUZZ_FLAGS    = -std=gnu11 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
		-DWITH_TLS=0 -DWITH_CONFIG=0 $(INC) -Itests/fuzz
FUZZ_SECONDS ?= 60

$(FUZZ_DIR)/fuzz_%: tests/fuzz/fuzz_%.c tests/fuzz/fuzz_common.h $(FUZZ_SRCS)
	@mkdir -p $(FUZZ_DIR)
	$(FUZZ_CC) $(FUZZ_FLAGS) $< $(FUZZ_SRCS) -o $@ -lm

fuzz: $(FUZZ_BINS)

fuzz-run: fuzz
	./tools/run_fuzz.sh $(FUZZ_SECONDS) $(FUZZ_NAMES)

# HTML reference of the sources in build/api/html (needs doxygen; warnings are errors).
api-docs:
	mkdir -p build/api && doxygen Doxyfile

clean:
	$(RM) -r $(DIST_DIR) build/api $(FUZZ_DIR)
	rm -f $(REV_STAMP)
	rm -f $(ALL_OBJS) $(ALL_DEPS) $(ALL_LIBS) $(BUILD_STAMP) $(BIN) $(TESTS) $(ASAN_BIN) $(ASAN_TESTS) \
		$(LIB_STATIC) $(LIB_SHARED) $(EXAMPLE_SO) $(EXAMPLE_A) $(TESTS).times $(ASAN_TESTS).times \
		python/mogbus/libmogbus.so

distclean: clean

re: clean all

commands:
	bear -- $(MAKE)

-include $(DEPS)

.PHONY: all dev lib python python-tests lib-check tests asan dist api-docs fuzz fuzz-run clean distclean re commands FORCE
