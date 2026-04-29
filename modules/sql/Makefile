# modules/sql/Makefile -- SQL (PostgreSQL + MySQL/MariaDB) binary module
#
# Builds the binary extension that scripts load via:
#
#     VAR sql = include("./sql.so")     # Linux  / macOS
#     VAR sql = include("./sql.dll")    # Windows
#
# Both wire-protocol drivers are written in pure C against the native
# protocols and statically linked into the resulting shared object;
# there are NO runtime dependencies on libpq, libmysqlclient, or any
# other third-party library.  OpenSSL is used for TLS + crypto via the
# same libssl/libcrypto that libcando itself already links, so a CanDo
# user can connect to either engine without installing anything.
#
# Targets:
#   all          sql.so + test_sql (default)
#   sql.so       POSIX / macOS shared library
#   sql.dll      Windows DLL via mingw-w64
#   test         build and run the C unit tests
#   clean        remove artefacts

ROOT      ?= ../..
CC        ?= gcc
MINGW_CC  ?= x86_64-w64-mingw32-gcc

INCLUDES   = -iquote $(ROOT)/source -iquote $(ROOT)/source/core \
             -iquote $(ROOT)/source/parser -iquote $(ROOT)/source/vm \
             -iquote $(ROOT)/source/object -iquote $(ROOT)/source/compat \
             -I$(ROOT)/include -I.

CFLAGS     = -std=c11 -Wall -Wextra -Wpedantic -fPIC $(INCLUDES)

# OS detection: pick the shared object name and link extras.
ifeq ($(OS),Windows_NT)
    SHARED   = sql.dll
    LDLIBS   = -lssl -lcrypto -lws2_32
else
    UNAME_S := $(shell uname -s)
    LDLIBS   = -lpthread -lm -ldl -lssl -lcrypto
    ifeq ($(UNAME_S),Darwin)
        SHARED = sql.dylib
    else
        SHARED = sql.so
    endif
endif

SRCS = sql_module.c sql_pg.c sql_mysql.c

.PHONY: all test clean sql.dll

all: $(SHARED) test_sql

# ---------------------------------------------------------------------------
# Linux / macOS shared library
# ---------------------------------------------------------------------------
sql.so: $(SRCS) sql_buf.h sql_crypto.h sql_driver.h sql_net.h sql_pg.h sql_mysql.h
	$(CC) $(CFLAGS) -shared $(SRCS) -o $@ $(LDLIBS)

sql.dylib: $(SRCS) sql_buf.h sql_crypto.h sql_driver.h sql_net.h sql_pg.h sql_mysql.h
	$(CC) $(CFLAGS) -dynamiclib -undefined dynamic_lookup \
	    $(SRCS) -o $@ $(LDLIBS)

# ---------------------------------------------------------------------------
# Windows DLL (cross-compile from Linux).
#
# Links:
#   - libcando.dll for the cando_bridge_* / cando_vm_* APIs
#   - OpenSSL libssl/libcrypto **statically** so users don't have to
#     ship libssl-3-x64.dll / libcrypto-3-x64.dll alongside sql.dll
#     (cando's own CI gate explicitly rejects DLLs that import them).
#     libcando.dll has its own private static copy too; sql.dll keeps
#     its own to avoid relying on private exports.
#   - winpthread + libgcc statically for the same reason; libgcc's spec
#     adds an implicit -lpthread that we have to neutralise via the
#     --whole-archive trick mirroring the top-level Makefile.
# ---------------------------------------------------------------------------
sql.dll: $(SRCS) sql_buf.h sql_crypto.h sql_driver.h sql_net.h sql_pg.h sql_mysql.h
	$(MINGW_CC) -std=c11 -Wall -Wextra -DCANDO_PLATFORM_WINDOWS \
	    -D_WIN32_WINNT=0x0600 \
	    -iquote $(ROOT)/source -iquote $(ROOT)/source/core \
	    -iquote $(ROOT)/source/parser -iquote $(ROOT)/source/vm \
	    -iquote $(ROOT)/source/object -iquote $(ROOT)/source/compat \
	    -I$(ROOT)/include -I. \
	    -shared $(SRCS) -o $@ \
	    -static-libgcc \
	    -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive \
	    -Wl,-Bstatic -lssl -lcrypto -Wl,-Bdynamic \
	    -L$(ROOT) -lcando -lws2_32 -lcrypt32 -lm \
	    -Wl,-Bstatic

# ---------------------------------------------------------------------------
# C unit tests for protocol-internal helpers (sql_buf.h, sql_crypto.h,
# protocol parsers).  These tests do NOT link against libcando -- they
# exercise the pieces that don't touch the VM.  Script-level coverage
# of the module surface lives in test_sql.cdo.
# ---------------------------------------------------------------------------
test_sql: test_sql.c sql_buf.h sql_crypto.h sql_driver.h
	$(CC) $(CFLAGS) -DSQL_MODULE_TEST_BUILD \
	    test_sql.c -o $@ -lssl -lcrypto -lpthread -lm

test: test_sql
	./test_sql

clean:
	rm -f sql.so sql.dylib sql.dll sql.lib test_sql
