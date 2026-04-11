# Makefile -- CanDo shared/static library and executable build
#
# Primary build system is CMake (CMakeLists.txt).  This Makefile is a
# convenience wrapper for developers who prefer GNU Make.
#
# Targets:
#   all            build libcando.so, libcando.a, and cando executable (default)
#   libcando.so    build shared library
#   libcando.a     build static library
#   cando          build the cando interpreter (links against libcando.so)
#   test           build all unit tests and run them
#   test_core      build and run core tests only
#   test_object    build and run object tests only
#   test_lexer     build and run lexer tests only
#   test_parser    build and run parser tests only
#   test_vm        build and run VM tests only
#   test_thread    build and run thread tests only
#   cando.exe      cross-compile cando.exe for Windows (requires mingw-w64)
#   libcando.dll   cross-compile shared library for Windows (requires mingw-w64)
#   clean          remove build artifacts

CC      = gcc

# -iquote: searched only for "quoted" includes, not <angle-bracket> includes.
# This prevents source/object/string.h from shadowing system <string.h>.
CFLAGS_CORE   = -std=c11 -Wall -Wextra -Wpedantic -pthread -D_GNU_SOURCE \
                -iquote source/core
CFLAGS_OBJECT = -std=c11 -Wall -Wextra -Wpedantic -pthread -D_GNU_SOURCE \
                -iquote source/core -iquote source/object
CFLAGS_PARSER = -std=c11 -Wall -Wextra -Wpedantic -pthread -D_GNU_SOURCE \
                -iquote source/core -iquote source/parser -iquote source/vm
# VM uses GCC computed-goto extension; suppress the pedantic warning.
CFLAGS_VM     = -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE \
                -iquote source/core -iquote source/vm -iquote source/object

# Flags for building libcando.so and libcando.a
# -DCANDO_BUILDING_LIB enables __attribute__((visibility("default"))) on exports.
CFLAGS_LIB = -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE \
             -DCANDO_BUILDING_LIB -fPIC \
             -iquote source -iquote source/core -iquote source/parser \
             -iquote source/vm -iquote source/object -iquote source/compat \
             -Iinclude

# Flags for the cando executable (links against libcando.so)
# -iquote source so cando.h's relative includes ("core/common.h" etc.) resolve.
CFLAGS_EXE = -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE \
             -iquote source -iquote source/core \
             -Iinclude

# OS detection for LDFLAGS
ifeq ($(OS),Windows_NT)
    LDFLAGS = -lm -lws2_32
else
    LDFLAGS = -lm -ldl
endif

# ---------------------------------------------------------------------------
# Source lists
# ---------------------------------------------------------------------------

CORE_SRCS = \
    source/core/common.c          \
    source/core/value.c           \
    source/core/lock.c            \
    source/core/handle.c          \
    source/core/memory.c          \
    source/core/thread_platform.c

OBJECT_SRCS = \
    source/object/string.c   \
    source/object/value.c    \
    source/object/object.c   \
    source/object/array.c    \
    source/object/function.c \
    source/object/class.c    \
    source/object/thread.c

LEXER_SRCS = $(CORE_SRCS) source/parser/lexer.c

PARSER_SRCS = $(LEXER_SRCS) source/parser/parser.c \
              source/vm/opcodes.c source/vm/chunk.c

VM_SRCS = \
    source/vm/opcodes.c \
    source/vm/chunk.c   \
    source/vm/bridge.c  \
    source/vm/vm.c      \
    source/vm/debug.c

# All library sources — everything compiled into libcando.so / libcando.a
CANDO_LIB_SRCS = \
    source/core/common.c          \
    source/core/value.c           \
    source/core/lock.c            \
    source/core/handle.c          \
    source/core/memory.c          \
    source/core/thread_platform.c \
    source/object/string.c        \
    source/object/value.c         \
    source/object/object.c        \
    source/object/array.c         \
    source/object/function.c      \
    source/object/class.c         \
    source/object/thread.c        \
    source/parser/lexer.c         \
    source/parser/parser.c        \
    source/vm/opcodes.c           \
    source/vm/chunk.c             \
    source/vm/bridge.c            \
    source/vm/vm.c                \
    source/vm/debug.c             \
    source/natives.c              \
    source/lib/math.c             \
    source/lib/file.c             \
    source/lib/eval.c             \
    source/lib/string.c           \
    source/lib/libutil.c          \
    source/lib/include.c          \
    source/lib/json.c             \
    source/lib/csv.c              \
    source/lib/thread.c           \
    source/lib/os.c               \
    source/lib/datetime.c         \
    source/lib/array.c            \
    source/lib/object.c           \
    source/lib/crypto.c           \
    source/lib/process.c          \
    source/lib/net.c              \
    source/cando_lib.c

# Windows compatibility shim added on Windows
CANDO_WIN_EXTRA = source/compat/win_regex.c

CANDO_BIN = cando

# ---------------------------------------------------------------------------
# Test binaries
# ---------------------------------------------------------------------------

TEST_CORE_BIN    = tests/test_core
TEST_OBJECT_BIN  = tests/test_object
TEST_LEXER_BIN   = tests/test_lexer
TEST_PARSER_BIN  = tests/test_parser
TEST_VM_BIN      = tests/test_vm
TEST_THREAD_BIN  = tests/test_thread

TEST_CORE_SRCS   = $(CORE_SRCS)   tests/test_core.c
TEST_OBJECT_SRCS = $(CORE_SRCS) $(OBJECT_SRCS) tests/test_object.c
TEST_LEXER_SRCS  = $(LEXER_SRCS)  tests/test_lexer.c
TEST_PARSER_SRCS = $(PARSER_SRCS) tests/test_parser.c
TEST_THREAD_SRCS = $(CORE_SRCS) $(OBJECT_SRCS) tests/test_thread.c

# ---------------------------------------------------------------------------
# Default target
# ---------------------------------------------------------------------------

.PHONY: all cando libcando.so libcando.a \
        test test_core test_object test_lexer test_parser test_vm test_thread \
        test_integration clean

all: libcando.so libcando.a $(CANDO_BIN) \
     $(TEST_CORE_BIN) $(TEST_OBJECT_BIN) $(TEST_LEXER_BIN) \
     $(TEST_PARSER_BIN) $(TEST_VM_BIN) $(TEST_THREAD_BIN)

# ---------------------------------------------------------------------------
# Shared library: libcando.so
# ---------------------------------------------------------------------------

libcando.so: $(CANDO_LIB_SRCS)
	$(CC) $(CFLAGS_LIB) -shared $^ -o $@ $(LDFLAGS)

# ---------------------------------------------------------------------------
# Static library: libcando.a
# Objects are compiled into a temporary directory to keep the repo clean.
# ---------------------------------------------------------------------------

LIBOBJS_DIR := .libobjs
LIBOBJS      = $(patsubst %.c,$(LIBOBJS_DIR)/%.o,$(CANDO_LIB_SRCS))

$(LIBOBJS_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_LIB) -c $< -o $@

libcando.a: $(LIBOBJS)
	ar rcs $@ $^

# ---------------------------------------------------------------------------
# Executable: cando  (links against libcando.so)
# ---------------------------------------------------------------------------

$(CANDO_BIN): source/main.c libcando.so
	$(CC) $(CFLAGS_EXE) source/main.c \
	    -L. -lcando -Wl,-rpath,'$$ORIGIN' \
	    -o $@ $(LDFLAGS)

cando: $(CANDO_BIN)

# ---------------------------------------------------------------------------
# Unit tests (compile from source — no dependency on libcando)
# ---------------------------------------------------------------------------

$(TEST_CORE_BIN): $(TEST_CORE_SRCS)
	$(CC) $(CFLAGS_CORE) $^ -o $@ $(LDFLAGS)

$(TEST_OBJECT_BIN): $(TEST_OBJECT_SRCS)
	$(CC) $(CFLAGS_OBJECT) $^ -o $@ $(LDFLAGS)

$(TEST_LEXER_BIN): $(TEST_LEXER_SRCS)
	$(CC) $(CFLAGS_PARSER) $^ -o $@ $(LDFLAGS)

$(TEST_PARSER_BIN): $(TEST_PARSER_SRCS)
	$(CC) $(CFLAGS_PARSER) $^ -o $@ $(LDFLAGS)

$(TEST_VM_BIN): $(CORE_SRCS) $(OBJECT_SRCS) $(VM_SRCS) tests/test_vm.c
	$(CC) $(CFLAGS_VM) $^ -o $@ $(LDFLAGS)

$(TEST_THREAD_BIN): $(TEST_THREAD_SRCS)
	$(CC) $(CFLAGS_OBJECT) $^ -o $@ $(LDFLAGS)

test: all
	./$(TEST_CORE_BIN)
	./$(TEST_OBJECT_BIN)
	./$(TEST_THREAD_BIN)
	./$(TEST_LEXER_BIN)
	./$(TEST_PARSER_BIN)
	./$(TEST_VM_BIN)
	bash tests/integration/run_tests.sh

test_integration: $(CANDO_BIN)
	bash tests/integration/run_tests.sh

test_core: $(TEST_CORE_BIN)
	./$(TEST_CORE_BIN)

test_object: $(TEST_OBJECT_BIN)
	./$(TEST_OBJECT_BIN)

test_lexer: $(TEST_LEXER_BIN)
	./$(TEST_LEXER_BIN)

test_parser: $(TEST_PARSER_BIN)
	./$(TEST_PARSER_BIN)

test_vm: $(TEST_VM_BIN)
	./$(TEST_VM_BIN)

test_thread: $(TEST_THREAD_BIN)
	./$(TEST_THREAD_BIN)

# ---------------------------------------------------------------------------
# Windows cross-compilation (requires mingw-w64)
# ---------------------------------------------------------------------------

MINGW_CC    = x86_64-w64-mingw32-gcc
WINDRES     = x86_64-w64-mingw32-windres

CFLAGS_WIN  = -std=c11 -Wall -Wextra -DCANDO_PLATFORM_WINDOWS -D_WIN32_WINNT=0x0600 \
              -DCANDO_BUILDING_LIB \
              -iquote source -iquote source/core -iquote source/parser -iquote source/vm \
              -iquote source/object -iquote source/compat \
              -Iinclude

CFLAGS_EXE_WIN = -std=c11 -Wall -Wextra -DCANDO_PLATFORM_WINDOWS -D_WIN32_WINNT=0x0600 \
                 -iquote source -iquote source/core -Iinclude

LDFLAGS_WIN = -lm -lws2_32

libcando.dll: $(CANDO_LIB_SRCS) $(CANDO_WIN_EXTRA)
	$(MINGW_CC) $(CFLAGS_WIN) -shared $^ -o $@ $(LDFLAGS_WIN) \
	    -Wl,--out-implib,libcando.lib

icon.res: source/icon.rc assets/icon.ico
	cd source && $(WINDRES) icon.rc -O coff -o ../icon.res

cando.exe: source/main.c libcando.dll icon.res
	$(MINGW_CC) $(CFLAGS_EXE_WIN) source/main.c icon.res \
	    -L. -lcando -o $@ $(LDFLAGS_WIN)

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------

clean:
	rm -f $(TEST_CORE_BIN) $(TEST_OBJECT_BIN) $(TEST_LEXER_BIN) \
	      $(TEST_PARSER_BIN) $(TEST_VM_BIN) $(TEST_THREAD_BIN) \
	      $(CANDO_BIN) cando.exe \
	      libcando.so libcando.a libcando.dll libcando.lib icon.res
	rm -rf $(LIBOBJS_DIR)
