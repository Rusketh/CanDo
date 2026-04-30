# modules/sqlite/Makefile -- SQLite binary module
#
# Builds the binary extension that scripts load via:
#
#     VAR sql = include("./sqlite.so")     # Linux  / macOS
#     VAR sql = include("./sqlite.dll")    # Windows
#
# SQLite itself is vendored as the amalgamation (vendor/sqlite3.c +
# vendor/sqlite3.h) and statically linked into the resulting shared
# object.  No system libsqlite is required at runtime.
#
# Targets:
#   all          sqlite.so + test_sqlite (default)
#   sqlite.so    POSIX / macOS shared library
#   sqlite.dll   Windows DLL via mingw-w64
#   test         build and run the C unit tests
#   clean        remove artefacts
#
# Variables:
#   CC           host C compiler                (default gcc)
#   MINGW_CC     Windows cross-compiler         (default x86_64-w64-mingw32-gcc)
#   ROOT         repo root, relative to this    (default ../..)

ROOT      ?= ../..
CC        ?= gcc
MINGW_CC  ?= x86_64-w64-mingw32-gcc

INCLUDES   = -iquote $(ROOT)/source -iquote $(ROOT)/source/core \
             -iquote $(ROOT)/source/parser -iquote $(ROOT)/source/vm \
             -iquote $(ROOT)/source/object -iquote $(ROOT)/source/compat \
             -I$(ROOT)/include -Ivendor

CFLAGS     = -std=c11 -Wall -Wextra -Wpedantic -fPIC $(INCLUDES)

# Compile-time options for the SQLite amalgamation.  These are also passed
# when the .c is built as part of the shared library, so the same flags
# apply to vendor/sqlite3.c whether on Linux or Windows.
SQLITE_CFLAGS = \
    -DSQLITE_THREADSAFE=1 \
    -DSQLITE_ENABLE_FTS5 \
    -DSQLITE_ENABLE_RTREE \
    -DSQLITE_ENABLE_JSON1 \
    -DSQLITE_ENABLE_MATH_FUNCTIONS \
    -DSQLITE_ENABLE_COLUMN_METADATA \
    -DSQLITE_DQS=0 \
    -DSQLITE_USE_URI=1 \
    -DSQLITE_OMIT_DEPRECATED \
    -DSQLITE_DEFAULT_FOREIGN_KEYS=1

# The amalgamation is upstream code -- silence its harmless warnings so
# our own -Wall -Wextra -Wpedantic still flags issues in the module.
SQLITE_QUIET = -Wno-unused-parameter -Wno-implicit-fallthrough \
               -Wno-cast-function-type -Wno-pedantic

# OS detection: pick the shared object name and link extras.
ifeq ($(OS),Windows_NT)
    SHARED   = sqlite.dll
    LDLIBS   =
else
    UNAME_S := $(shell uname -s)
    LDLIBS   = -lpthread -lm -ldl
    ifeq ($(UNAME_S),Darwin)
        SHARED = sqlite.dylib
    else
        SHARED = sqlite.so
    endif
endif

.PHONY: all test clean sqlite.dll

all: $(SHARED) test_sqlite

# ---------------------------------------------------------------------------
# Vendored amalgamation -- compiled to a .o once, reused by both the .so
# and the test binary.
# ---------------------------------------------------------------------------
vendor/sqlite3.o: vendor/sqlite3.c vendor/sqlite3.h
	$(CC) -std=c11 -fPIC -O2 $(SQLITE_CFLAGS) $(SQLITE_QUIET) \
	    -c vendor/sqlite3.c -o vendor/sqlite3.o

# ---------------------------------------------------------------------------
# Linux / macOS shared library
# ---------------------------------------------------------------------------
sqlite.so: sqlite_module.c vendor/sqlite3.o
	$(CC) $(CFLAGS) -shared sqlite_module.c vendor/sqlite3.o \
	    -o $@ $(LDLIBS)

sqlite.dylib: sqlite_module.c vendor/sqlite3.o
	$(CC) $(CFLAGS) -dynamiclib -undefined dynamic_lookup \
	    sqlite_module.c vendor/sqlite3.o -o $@ $(LDLIBS)

# ---------------------------------------------------------------------------
# Windows DLL (cross-compile from Linux).  vendor/sqlite3.c is compiled
# inline so it picks up the MinGW definitions automatically; no system
# sqlite required.
# ---------------------------------------------------------------------------
sqlite.dll: sqlite_module.c vendor/sqlite3.c vendor/sqlite3.h
	$(MINGW_CC) -std=c11 -Wall -Wextra -DCANDO_PLATFORM_WINDOWS \
	    -D_WIN32_WINNT=0x0600 \
	    $(SQLITE_CFLAGS) $(SQLITE_QUIET) \
	    -iquote $(ROOT)/source -iquote $(ROOT)/source/core \
	    -iquote $(ROOT)/source/parser -iquote $(ROOT)/source/vm \
	    -iquote $(ROOT)/source/object -iquote $(ROOT)/source/compat \
	    -I$(ROOT)/include -Ivendor \
	    -shared sqlite_module.c vendor/sqlite3.c -o $@ \
	    -L$(ROOT) -lcando

# ---------------------------------------------------------------------------
# C unit tests for module-internal helpers (chunk 1: nothing yet)
# ---------------------------------------------------------------------------
test_sqlite: test_sqlite.c vendor/sqlite3.o
	$(CC) $(CFLAGS) $(SQLITE_QUIET) -DSQLITE_MODULE_TEST_BUILD \
	    test_sqlite.c vendor/sqlite3.o -o $@ $(LDLIBS)

test: test_sqlite
	./test_sqlite

clean:
	rm -f sqlite.so sqlite.dylib sqlite.dll sqlite.lib test_sqlite \
	      vendor/sqlite3.o
