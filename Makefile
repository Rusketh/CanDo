# Makefile -- Cando core, lexer, parser, VM, and executable build
#
# Targets:
#   all           build all test binaries and cando executable (default)
#   cando         build the cando script interpreter executable
#   test          build and run all tests
#   test_core     build and run core tests only
#   test_object   build and run object tests only
#   test_lexer    build and run lexer tests only
#   test_parser   build and run parser tests only
#   test_vm       build and run VM tests only
#   clean         remove build artifacts

CC      = gcc

# -iquote: searched only for "quoted" includes, not <angle-bracket> includes.
# This prevents source/object/string.h from shadowing system <string.h>.
CFLAGS_CORE   = -std=c11 -Wall -Wextra -Wpedantic -pthread -D_GNU_SOURCE \
                -iquote source/core
CFLAGS_OBJECT = -std=c11 -Wall -Wextra -Wpedantic -pthread -D_GNU_SOURCE \
                -iquote source/core -iquote source/object
CFLAGS_PARSER = -std=c11 -Wall -Wextra -Wpedantic -pthread -D_GNU_SOURCE \
                -iquote source/core -iquote source/parser -iquote source/vm
# cando executable: core + parser + VM + natives + object
CFLAGS_CANDO  = -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE \
                -iquote source/core -iquote source/parser -iquote source/vm \
                -iquote source/object -iquote source
# VM uses GCC computed-goto extension; suppress the pedantic warning.
CFLAGS_VM     = -std=c11 -Wall -Wextra -pthread -D_GNU_SOURCE \
                -I source/core -I source/vm -iquote source/object

# OS detection for LDFLAGS
ifeq ($(OS),Windows_NT)
    LDFLAGS = -lm -lws2_32
else
    LDFLAGS = -lm -ldl
endif

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

CANDO_WIN_EXTRA = source/compat/win_regex.c

CANDO_SRCS = \
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
    source/parser/lexer.c     \
    source/parser/parser.c    \
    source/vm/opcodes.c       \
    source/vm/chunk.c         \
    source/vm/bridge.c        \
    source/vm/vm.c            \
    source/vm/debug.c         \
    source/natives.c          \
    source/lib/math.c         \
    source/lib/file.c         \
    source/lib/eval.c         \
    source/lib/string.c       \
    source/lib/libutil.c      \
    source/lib/include.c      \
    source/lib/json.c         \
    source/lib/csv.c          \
    source/lib/thread.c       \
    source/lib/os.c           \
    source/lib/datetime.c     \
    source/lib/array.c        \
    source/lib/crypto.c       \
    source/lib/process.c      \
    source/lib/net.c          \
    source/main.c

CANDO_BIN = cando

VM_SRCS = \
    source/vm/opcodes.c \
    source/vm/chunk.c   \
    source/vm/bridge.c  \
    source/vm/vm.c      \
    source/vm/debug.c

# --- test binaries ---
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

.PHONY: all cando test test_core test_object test_lexer test_parser test_vm test_thread test_integration clean

all: $(TEST_CORE_BIN) $(TEST_OBJECT_BIN) $(TEST_LEXER_BIN) $(TEST_PARSER_BIN) $(TEST_VM_BIN) $(TEST_THREAD_BIN) $(CANDO_BIN)

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

$(CANDO_BIN): $(CANDO_SRCS)
	$(CC) $(CFLAGS_CANDO) $^ -o $@ $(LDFLAGS)

cando: $(CANDO_BIN)

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

# Windows cross-compilation (requires mingw-w64)
MINGW_CC    = x86_64-w64-mingw32-gcc
CFLAGS_WIN  = -std=c11 -Wall -Wextra -DCANDO_PLATFORM_WINDOWS -D_WIN32_WINNT=0x0600 \
              -iquote source/core -iquote source/parser -iquote source/vm \
              -iquote source/object -iquote source -iquote source/compat
LDFLAGS_WIN = -lm -lws2_32

cando.exe: $(CANDO_SRCS) $(CANDO_WIN_EXTRA)
	$(MINGW_CC) $(CFLAGS_WIN) $^ -o $@ $(LDFLAGS_WIN)

clean:
	rm -f $(TEST_CORE_BIN) $(TEST_OBJECT_BIN) $(TEST_LEXER_BIN) $(TEST_PARSER_BIN) $(TEST_VM_BIN) $(TEST_THREAD_BIN) $(CANDO_BIN) cando.exe
