# modules/smtp/Makefile -- CanDo SMTP / IMAP / POP3 / MIME module
#
# Builds the binary extension that scripts load via:
#
#     VAR mail = include("./smtp.so")     # Linux / macOS
#     VAR mail = include("./smtp.dll")    # Windows
#
# Targets:
#   all        smtp.so + test_smtp (default)
#   smtp.so    POSIX / macOS shared library (OpenSSL + libresolv)
#   smtp.dll   Windows DLL via mingw-w64 (Schannel via OpenSSL + dnsapi)
#   test       build and run the C unit tests
#   clean      remove artefacts
#
# Variables:
#   CC         host C compiler                (default gcc)
#   MINGW_CC   Windows cross-compiler          (default x86_64-w64-mingw32-gcc)
#   ROOT       repo root, relative to this    (default ../..)

ROOT      ?= ../..
CC        ?= gcc
MINGW_CC  ?= x86_64-w64-mingw32-gcc

INCLUDES   = -iquote $(ROOT)/source -iquote $(ROOT)/source/core \
             -iquote $(ROOT)/source/parser -iquote $(ROOT)/source/vm \
             -iquote $(ROOT)/source/object -iquote $(ROOT)/source/compat \
             -iquote $(ROOT)/source/lib \
             -I$(ROOT)/include

CFLAGS     = -std=c11 -Wall -Wextra -Wno-unused-parameter \
             -Wno-unused-function -Wno-format-truncation \
             -fPIC $(INCLUDES)

ifeq ($(OS),Windows_NT)
    LDLIBS    = -lssl -lcrypto -lws2_32 -ldnsapi
    SHARED    = smtp.dll
else
    UNAME_S  := $(shell uname -s)
    LDLIBS    = -lssl -lcrypto
    ifeq ($(UNAME_S),Darwin)
        SHARED = smtp.dylib
        # macOS uses libresolv built into libSystem.
        LDLIBS += -lresolv
    else
        SHARED = smtp.so
        LDLIBS += -lresolv
    endif
endif

.PHONY: all test clean smtp.dll

all: $(SHARED) test_smtp

# ---------------------------------------------------------------------------
# Linux / macOS shared library
# ---------------------------------------------------------------------------
smtp.so: smtp_module.c smtp_helpers.h mime.h dns.h dkim.h spf.h storage.h
	$(CC) $(CFLAGS) -shared smtp_module.c -o $@ $(LDLIBS)

smtp.dylib: smtp_module.c smtp_helpers.h mime.h dns.h dkim.h spf.h storage.h
	$(CC) $(CFLAGS) -dynamiclib -undefined dynamic_lookup smtp_module.c -o $@ $(LDLIBS)

# ---------------------------------------------------------------------------
# Windows DLL (cross-compile from Linux)
# ---------------------------------------------------------------------------
smtp.dll: smtp_module.c smtp_helpers.h mime.h dns.h dkim.h spf.h storage.h
	$(MINGW_CC) -std=c11 -Wall -Wextra -Wno-unused-parameter \
	    -Wno-unused-function -Wno-format-truncation \
	    -DCANDO_PLATFORM_WINDOWS -D_WIN32_WINNT=0x0600 \
	    -iquote $(ROOT)/source -iquote $(ROOT)/source/core \
	    -iquote $(ROOT)/source/parser -iquote $(ROOT)/source/vm \
	    -iquote $(ROOT)/source/object -iquote $(ROOT)/source/compat \
	    -iquote $(ROOT)/source/lib \
	    -I$(ROOT)/include \
	    -shared smtp_module.c -o $@ \
	    -L$(ROOT) -lcando \
	    -Wl,-Bstatic -lssl -lcrypto -Wl,-Bdynamic \
	    -lws2_32 -ldnsapi -lcrypt32

# ---------------------------------------------------------------------------
# C unit tests for module-internal helpers
# ---------------------------------------------------------------------------
test_smtp: test_smtp.c smtp_helpers.h mime.h
	$(CC) $(CFLAGS) -DSMTP_MODULE_TEST_BUILD test_smtp.c -o $@ $(LDLIBS)

test: test_smtp
	./test_smtp

clean:
	rm -f smtp.so smtp.dylib smtp.dll smtp.lib test_smtp
