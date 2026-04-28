# modules/ldap/Makefile -- LDAP / Active Directory module
#
# Builds the binary extension that scripts load via:
#
#     VAR ldap = include("./ldap.so")     # Linux  / macOS
#     VAR ldap = include("./ldap.dll")    # Windows
#
# Targets:
#   all        ldap.so + test_ldap (default)
#   ldap.so    POSIX / macOS shared library (libldap)
#   ldap.dll   Windows DLL via mingw-w64 (wldap32)
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
             -I$(ROOT)/include

CFLAGS     = -std=c11 -Wall -Wextra -Wpedantic -fPIC $(INCLUDES)

# OS detection: pick libldap on POSIX, wldap32 on MinGW.
ifeq ($(OS),Windows_NT)
    LDLIBS    = -lwldap32
    SHARED    = ldap.dll
else
    UNAME_S  := $(shell uname -s)
    LDLIBS    = -lldap -llber
    ifeq ($(UNAME_S),Darwin)
        SHARED = ldap.dylib
    else
        SHARED = ldap.so
    endif
endif

.PHONY: all test clean ldap.dll

all: $(SHARED) test_ldap

# ---------------------------------------------------------------------------
# Linux / macOS shared library
# ---------------------------------------------------------------------------
ldap.so: ldap_module.c
	$(CC) $(CFLAGS) -shared $< -o $@ $(LDLIBS)

ldap.dylib: ldap_module.c
	$(CC) $(CFLAGS) -dynamiclib -undefined dynamic_lookup $< -o $@ $(LDLIBS)

# ---------------------------------------------------------------------------
# Windows DLL (cross-compile from Linux)
# ---------------------------------------------------------------------------
ldap.dll: ldap_module.c
	$(MINGW_CC) -std=c11 -Wall -Wextra -DCANDO_PLATFORM_WINDOWS \
	    -D_WIN32_WINNT=0x0600 \
	    -iquote $(ROOT)/source -iquote $(ROOT)/source/core \
	    -iquote $(ROOT)/source/parser -iquote $(ROOT)/source/vm \
	    -iquote $(ROOT)/source/object -iquote $(ROOT)/source/compat \
	    -I$(ROOT)/include \
	    -shared $< -o $@ \
	    -L$(ROOT) -lcando -lwldap32

# ---------------------------------------------------------------------------
# C unit tests for module-internal helpers
# ---------------------------------------------------------------------------
test_ldap: test_ldap.c ldap_module.c
	$(CC) $(CFLAGS) -DLDAP_MODULE_TEST_BUILD test_ldap.c -o $@ $(LDLIBS)

test: test_ldap
	./test_ldap

clean:
	rm -f ldap.so ldap.dylib ldap.dll ldap.lib test_ldap
