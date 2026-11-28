ifeq ($(wildcard vlib/arena.h),)
$(error vlib submodule missing: run git submodule update --init)
endif

# mogbus build. Modeled on the Makefile vlib/gen_makefile.py generates, but the
# source tree has sub-folders (one per module), so sources are found recursively.

# OS layer: src/os/os_*_<PLATFORM>.c are the only per-platform sources. PLATFORM=win32 cross-compiles
# a native Windows build with MinGW-w64 (CROSS is the toolchain prefix); see docs/user/windows/windows-route.md.
PLATFORM    ?= posix
ifeq ($(PLATFORM),posix)
EXE         =
OTHER_PLATFORM_SRCS = $(SRC_DIR)/os/%_win32.c
else ifeq ($(PLATFORM),win32)
CROSS       ?= x86_64-w64-mingw32-
EXE         = .exe
OTHER_PLATFORM_SRCS = $(SRC_DIR)/os/%_posix.c
ifeq ($(origin AR),default)
AR          = $(CROSS)ar
endif
else
$(error PLATFORM must be posix or win32 (got '$(PLATFORM)'))
endif

BIN         = mogbus$(EXE)
TESTS       = mogbus-tests$(EXE)
ASAN_BIN    = mogbus-asan
ASAN_TESTS  = mogbus-asan-tests
ifeq ($(PLATFORM),win32)
LIB_STATIC  = libmogbus.win32.a
LIB_SHARED  = mogbus.dll
LIB_IMPORT  = libmogbus.dll.a
else
LIB_STATIC  = libmogbus.a
LIB_SHARED  = libmogbus.so
endif
EXAMPLE_SRC = examples/embed_tcp.c
EXAMPLE_SO  = examples/embed_tcp_shared
EXAMPLE_A   = examples/embed_tcp_static
CLIENT_SRC  = examples/client_function_code.c
CLIENT_SO   = examples/client_function_code_shared
CLIENT_A    = examples/client_function_code_static
TUTORIAL_SRC = examples/tutorial/pump_station.c
TUTORIAL_SO  = examples/tutorial/pump_station_shared
TUTORIAL_A   = examples/tutorial/pump_station_static
# The tutorial program also loads a profile when the build has the config loader.
TUTORIAL_ARGS = --self-test $(if $(filter 1,$(WITH_CONFIG)),--unit 10 --config profiles/two-devices.scm)

# Prefer clang, fall back to gcc then cc. ccache and mold are independent
# speed-ups, each used only when installed.
# GNU make predefines CC with origin 'default'. Replace only that built-in value:
# an explicit CC from the environment or command line must still win.
ifeq ($(origin CC),default)
ifeq ($(PLATFORM),win32)
DEFAULT_CC  := $(CROSS)gcc
else
DEFAULT_CC  := $(shell if command -v clang >/dev/null 2>&1; then echo clang; \
                       elif command -v gcc >/dev/null 2>&1; then echo gcc; \
                       else echo cc; fi)
endif
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

# Windows has no PIC (code is position independent by relocation) and exports through dllexport.
# MinGW's printf format attribute means the old MSVCRT dialect (no %zu) even with the C99 stdio
# switched on, so format checking stays with the Linux build (-Wno-format after -Wformat=2).
ifeq ($(PLATFORM),win32)
PIC_FLAGS   = -DMOGBUS_BUILD -D__USE_MINGW_ANSI_STDIO=1 -Wno-format -Wno-format-extra-args
else
PIC_FLAGS   = -fPIC
endif
CFLAGS      ?= -std=gnu11 $(OPT_FLAGS) -Werror -Wextra -Wall -Wformat=2 -Wshadow $(FORTIFY_FLAGS) -fstack-protector-strong $(PIC_FLAGS) -fvisibility=hidden
# -fPIC (not -fPIE) so the same objects serve the executables, libmogbus.a and
# libmogbus.so; PIC code is valid in a PIE and costs next to nothing on x86-64.
# -fvisibility=hidden leaves only the MOGBUS_API symbols exported from the .so.
# Hardening applies to every link, even when LDFLAGS is overridden; skip flags
# the user already supplied so overrides stay free of duplicates.
ifeq ($(PLATFORM),win32)
# ASLR (dynamicbase, high-entropy-va) and DEP (nxcompat); static so the .exe needs no MinGW DLLs.
HARDEN_LDFLAGS = -static -Wl,--dynamicbase,--nxcompat,--high-entropy-va
else
HARDEN_LDFLAGS = -pie -Wl,-z,relro,-z,now
endif
# Probe the chosen compiler (including a ccache prefix) before adding mold; old
# gcc versions reject -fuse-ld=mold. An explicit LDFLAGS skips the probe entirely.
ifeq ($(origin LDFLAGS),undefined)
ifeq ($(PLATFORM),posix)
MOLD_LDFLAGS := $(shell if command -v mold >/dev/null 2>&1 && \
                            printf 'int main(void){return 0;}\n' | $(CC) -x c - -fuse-ld=mold -o /dev/null >/dev/null 2>&1; then \
                        echo -fuse-ld=mold; \
                 fi)
endif
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
ifeq ($(PLATFORM),win32)
LIBS        += -lws2_32
endif
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

PLAT_OBJ_TAG =
ifeq ($(PLATFORM),win32)
PLAT_OBJ_TAG = .win32
endif

# SAN=1 and BUILD=dev build separate objects and link their runtimes/flags.
SAN ?= 0

ifeq ($(SAN),1)
OBJEXT      = $(DEV_OBJ_TAG)$(TLS_OBJ_TAG)$(CONFIG_OBJ_TAG)$(PLAT_OBJ_TAG).asan.o
BIN         = $(ASAN_BIN)
TESTS       = $(ASAN_TESTS)
SAN_FLAGS   = -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=undefined,float-cast-overflow -fno-omit-frame-pointer
else
OBJEXT      = $(DEV_OBJ_TAG)$(TLS_OBJ_TAG)$(CONFIG_OBJ_TAG)$(PLAT_OBJ_TAG).o
SAN_FLAGS   =
endif

SRC_DIR     = src
TREE_SRCS   = $(filter-out $(OTHER_PLATFORM_SRCS),$(shell find $(SRC_DIR) -name '*.c' ! -name main.c | LC_ALL=C sort))
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
TEST_SRCS   := $(filter-out tests/actions.c tests/config.c tests/ascii_cli.c tests/decode_cli.c tests/rtu_cli.c tests/show_registers_cli.c tests/multi_server_cli.c tests/tcp_server.c \
                            tests/machine_e2e.c tests/ascii_tcp_server.c tests/rtu_tcp_server.c \
                            tests/udp_server.c tests/%_udp_server.c,$(TEST_SRCS))
endif
# These fork, use PTYs, termios, poll() or POSIX sockets directly, or spawn the binary through
# tests_utils/test_support.c; they stay out of the Windows test binary (docs/dev/architecture.md).
TESTS_POSIX_ONLY = tests/api.c tests/poll_fds.c tests/client.c tests/client_calls.c tests/ascii_cli.c tests/ascii_tcp_server.c tests/ascii_udp_server.c \
                   tests/decode_cli.c tests/machine_e2e.c tests/robustness.c tests/rtu_cli.c tests/rtu_server.c tests/show_registers_cli.c tests/multi_server_cli.c \
                   tests/rtu_tcp_server.c tests/rtu_udp_server.c tests/serial.c \
                   tests/server_facade.c tests/server_objects.c tests/tcp_server.c tests/threads.c tests/udp_server.c \
                   tests/tests_utils/test_support.c tests/tests_utils/tls_client.c
ifeq ($(PLATFORM),win32)
TEST_SRCS   := $(filter-out $(TESTS_POSIX_ONLY),$(TEST_SRCS))
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
ALL_OBJ_SUFFIXES = $(foreach d,.- .dev,$(foreach t,.- .notls,$(foreach c,.- .nocfg,$(foreach p,.- .win32,$(foreach a,.o .asan.o,$(subst .-,,$d)$(subst .-,,$t)$(subst .-,,$c)$(subst .-,,$p)$a)))))
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

# embed.py, pump_station.py and the unittest tests need only the library; test_device.py, test_station.py
# and conftest.py need pytest and are not run here.
python-examples: $(LIB_SHARED)
	MOGBUS_LIB=$(CURDIR)/$(LIB_SHARED) PYTHONPATH=python python3 examples/python/embed.py
	MOGBUS_LIB=$(CURDIR)/$(LIB_SHARED) PYTHONPATH=python python3 examples/python/asyncio_server.py
	MOGBUS_LIB=$(CURDIR)/$(LIB_SHARED) PYTHONPATH=python python3 examples/python/pump_station.py
	MOGBUS_LIB=$(CURDIR)/$(LIB_SHARED) PYTHONPATH=python python3 -m unittest discover -s examples/python -p test_pump_station.py

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

# `make tests` runs the scopes selected by WHAT (comma-separated: c, python, docs, all, auto;
# tools/changes.sh explains its choice on stderr). auto picks scopes from the changed files;
# when CI is set the default is c. SUITES (comma-separated vtest suite names, empty = all) limits
# the c scope.
ifdef CI
WHAT ?= c
else
WHAT ?= auto
endif
SUITES ?=
ifneq ($(filter tests,$(MAKECMDGOALS)),)
TEST_SCOPES := $(shell ./tools/changes.sh scope '$(WHAT)')
ifneq ($(.SHELLSTATUS),0)
$(error invalid WHAT)
endif
endif
tests: $(addprefix tests-,$(TEST_SCOPES))
tests-python: python-tests python-examples $(PROGRAMS)
tests-docs: docs $(PROGRAMS)
	./tools/check_docs.sh examples ./$(BIN)

# Windows tests run under Wine; the toolchain shims and the nm export check are Linux-only.
ifeq ($(PLATFORM),win32)
WINE        ?= wine
tests-c: $(TESTS) $(PROGRAMS)
	WINEDEBUG=-all RUN_TESTS_WRAPPER='$(WINE)' RUN_TESTS_SUITES='$(SUITES)' MOGBUS_BIN=./$(BIN) ./tools/run_tests.sh ./$(TESTS)
lib-check:
	@echo "lib-check: skipped for PLATFORM=win32 (nm -D export check is Linux-only)"
else
tests-c: $(TESTS) $(PROGRAMS) lib-check
ifeq ($(WITH_CONFIG),1)
	./tools/check_toolchain.sh
endif
	RUN_TESTS_SUITES='$(SUITES)' ./tools/run_tests.sh ./$(TESTS)
endif

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
ifeq ($(PLATFORM),win32)
# mogbus.dll plus its import library; libgcc is linked statically so the DLL needs nothing from MinGW.
$(LIB_SHARED): $(BUILD_STAMP) $(LIB_OBJS)
	$(CC) $(CFLAGS) $(REPRO_FLAGS) -shared -static-libgcc -Wl,--out-implib,$(LIB_IMPORT) $(LDFLAGS) -Wl,--dynamicbase,--nxcompat,--high-entropy-va $(filter-out $(BUILD_STAMP),$^) -o $@ $(LIBS)
else
$(LIB_SHARED): $(BUILD_STAMP) $(LIB_OBJS)
	$(CC) $(CFLAGS) $(REPRO_FLAGS) -shared -Wl,-soname,$@ $(filter-out -pie,$(LDFLAGS)) -Wl,-z,relro,-z,now $(filter-out $(BUILD_STAMP),$^) -o $@ $(LIBS)
endif

# The example sees only include/mogbus.h, which proves the public header and the
# exported symbols are enough. The .so is found through an $ORIGIN rpath.
$(EXAMPLE_SO): $(EXAMPLE_SRC) include/mogbus.h $(LIB_SHARED)
	$(CC) $(CFLAGS) -Iinclude $< ./$(LIB_SHARED) $(LINK_FLAGS) -Wl,-rpath,'$$ORIGIN/..' -o $@

$(EXAMPLE_A): $(EXAMPLE_SRC) include/mogbus.h $(LIB_STATIC)
	$(CC) $(CFLAGS) -Iinclude $< $(LIB_STATIC) $(LINK_FLAGS) -o $@ $(LIBS)

# Builds the libraries, requires the .so to export only mogbus_* symbols, and runs
# the example against both.
ifeq ($(PLATFORM),posix)
$(CLIENT_SO): $(CLIENT_SRC) include/mogbus.h $(LIB_SHARED)
	$(CC) $(CFLAGS) -Iinclude $< ./$(LIB_SHARED) $(LINK_FLAGS) -Wl,-rpath,'$$ORIGIN/..' -o $@

$(CLIENT_A): $(CLIENT_SRC) include/mogbus.h $(LIB_STATIC)
	$(CC) $(CFLAGS) -Iinclude $< $(LIB_STATIC) $(LINK_FLAGS) -o $@ $(LIBS)

$(TUTORIAL_SO): $(TUTORIAL_SRC) include/mogbus.h $(LIB_SHARED)
	$(CC) $(CFLAGS) -Iinclude $< ./$(LIB_SHARED) $(LINK_FLAGS) -Wl,-rpath,'$$ORIGIN/../..' -o $@

$(TUTORIAL_A): $(TUTORIAL_SRC) include/mogbus.h $(LIB_STATIC)
	$(CC) $(CFLAGS) -Iinclude $< $(LIB_STATIC) $(LINK_FLAGS) -o $@ $(LIBS)

lib-check: $(EXAMPLE_SO) $(EXAMPLE_A) $(CLIENT_SO) $(CLIENT_A) $(TUTORIAL_SO) $(TUTORIAL_A)
	@bad=$$(nm -D --defined-only $(LIB_SHARED) | awk '{print $$3}' | \
		grep -vE '^(mogbus_|_init$$|_fini$$|_edata$$|_end$$|__bss_start$$)' || true); \
	if [ -n "$$bad" ]; then echo "$(LIB_SHARED) exports non-API symbols:" $$bad >&2; exit 1; fi; \
	nm -D --defined-only $(LIB_SHARED) | grep -q ' mogbus_create$$' || { echo "$(LIB_SHARED) lacks mogbus_create" >&2; exit 1; }
	./$(EXAMPLE_SO)
	./$(EXAMPLE_A)
	./$(CLIENT_SO)
	./$(CLIENT_A)
	./$(TUTORIAL_SO) $(TUTORIAL_ARGS) > /dev/null
	./$(TUTORIAL_A) $(TUTORIAL_ARGS) > /dev/null
endif

$(TESTS): $(BUILD_STAMP) $(TEST_OBJS) $(CFG_S7_OBJ)
	$(CC) $(CFLAGS) $(SAN_FLAGS) $(LINK_FLAGS) $(filter-out $(BUILD_STAMP),$^) -o $@ $(LIBS)

# The web page becomes a C array with POSIX tools only (od, awk): the same bytes on every host, no
# paths or timestamps. It is included by src/webui/webui.c and removed by `make clean`.
WEBUI_DATA  = src/webui/page_data.h
$(WEBUI_DATA): src/webui/index.html
	od -An -v -tu1 $< | awk 'BEGIN { print "/** @file\n * @brief Generated from index.html by the Makefile: the web page bytes. */"; print "static const unsigned char webui_index_html[] = {" } \
		{ for (i = 1; i <= NF; i++) { printf "%s,", $$i; if (++n % 20 == 0) printf "\n" } } \
		END { print "\n};" }' > $@.tmp
	mv $@.tmp $@

src/webui/webui$(OBJEXT): $(WEBUI_DATA)

$(S7_OBJ): third_party/s7/s7.c third_party/s7/s7.h
	$(CC) $(S7_FLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_STAMP): FORCE
	@printf '%s\n' '$(BUILD) tls=$(WITH_TLS) config=$(WITH_CONFIG) platform=$(PLATFORM)' | cmp -s - $@ 2>/dev/null || \
		printf '%s\n' '$(BUILD) tls=$(WITH_TLS) config=$(WITH_CONFIG) platform=$(PLATFORM)' > $@

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
ifneq ($(BUILD)$(SAN)$(WITH_CONFIG)$(PLATFORM),prod01posix)
	$(error make dist needs BUILD=prod, SAN=0, WITH_CONFIG=1 and PLATFORM=posix)
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
	cp -r docs/user/. $$stage/share/doc/mogbus/docs/ && cp profiles/*.scm $$stage/share/mogbus/profiles/ && \
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
FUZZ_SRCS     = $(sort $(wildcard src/modbus/*.c src/decode/*.c src/device/*.c src/util/*.c src/os/*_$(PLATFORM).c)) \
		src/net/net_common.c src/net/net_control.c src/net/net_obs.c src/net/rtu_framer.c src/webui/webui.c
FUZZ_FLAGS    = -std=gnu11 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
		-DWITH_TLS=0 -DWITH_CONFIG=0 $(INC) -Itests/fuzz
FUZZ_SECONDS ?= 60

$(FUZZ_DIR)/fuzz_%: tests/fuzz/fuzz_%.c tests/fuzz/fuzz_common.h $(FUZZ_SRCS) $(WEBUI_DATA)
	@mkdir -p $(FUZZ_DIR)
	$(FUZZ_CC) $(FUZZ_FLAGS) $< $(FUZZ_SRCS) -o $@ -lm

fuzz: $(FUZZ_BINS)

fuzz-run: fuzz
	python3 tools/fuzz.py $(FUZZ_SECONDS) $(FUZZ_NAMES)

# Documentation site in build/docs (needs doxygen and python3; warnings are errors): Doxygen XML
# and the source browser (build/docs/internals), a check that every file, struct, union, enum and
# opaque typedef has a brief, then tools/docs/build.py renders the site from the XML.
docs:
	mkdir -p build/docs && doxygen Doxyfile
	./tools/check_docs.sh briefs
	python3 tools/docs/build.py

clean:
	$(RM) -r $(DIST_DIR) build/docs $(FUZZ_DIR)
	rm -f $(REV_STAMP) $(WEBUI_DATA) $(WEBUI_DATA).tmp
	rm -f $(ALL_OBJS) $(ALL_DEPS) $(ALL_LIBS) $(BUILD_STAMP) $(BIN) $(TESTS) $(ASAN_BIN) $(ASAN_TESTS) \
		$(LIB_STATIC) $(LIB_SHARED) $(EXAMPLE_SO) $(EXAMPLE_A) $(CLIENT_SO) $(CLIENT_A) $(TUTORIAL_SO) $(TUTORIAL_A) $(TESTS).times $(ASAN_TESTS).times \
		python/mogbus/libmogbus.so mogbus mogbus.exe mogbus-tests mogbus-tests.exe mogbus-tests.exe.times mogbus.dll libmogbus.dll.a

distclean: clean

re: clean all

commands:
	bear -- $(MAKE)

-include $(DEPS)

.PHONY: all dev lib python python-tests python-examples lib-check tests tests-c tests-python tests-docs asan dist docs fuzz fuzz-run clean distclean re commands FORCE
