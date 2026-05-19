# modules/cffi/Makefile -- C foreign-function-interface binary module
#
# Builds the binary extension that scripts load via:
#
#     VAR ffi = include("./cffi.so");        # Linux  / macOS
#     VAR ffi = include("./cffi.dll");       # Windows
#
# Depends on libffi for ABI-correct calls and dlopen / LoadLibrary
# for shared-library loading.  libffi is a hard runtime dependency:
# - Linux:    apt install libffi-dev   (Ubuntu) / dnf install libffi-devel
# - macOS:    ships in the SDK (no install needed)
# - Windows:  MSYS2:  pacman -S mingw-w64-x86_64-libffi
#
# Targets:
#   all          cffi.so + test_cffi (default)
#   cffi.so      POSIX / macOS shared library
#   cffi.dll     Windows DLL via mingw-w64
#   test         build and run the C unit tests
#   clean        remove artefacts
#
# Variables:
#   CC           host C compiler                 (default gcc)
#   MINGW_CC     Windows cross-compiler          (default x86_64-w64-mingw32-gcc)
#   ROOT         repo root, relative to this     (default ../..)

ROOT      ?= ../..
CC        ?= gcc
MINGW_CC  ?= x86_64-w64-mingw32-gcc

INCLUDES   = -iquote $(ROOT)/source -iquote $(ROOT)/source/core \
             -iquote $(ROOT)/source/parser -iquote $(ROOT)/source/vm \
             -iquote $(ROOT)/source/object -iquote $(ROOT)/source/compat \
             -I$(ROOT)/include

CFLAGS     = -std=c11 -Wall -Wextra -Wpedantic -fPIC $(INCLUDES)

# OS detection: pick the shared-object name, libffi link variant, and
# any extra system libraries.
ifeq ($(OS),Windows_NT)
    SHARED   = cffi.dll
    FFI_LIBS = -lffi
    LDLIBS   = $(FFI_LIBS)
else
    UNAME_S := $(shell uname -s)
    FFI_LIBS = -lffi
    LDLIBS   = $(FFI_LIBS) -lpthread -lm -ldl
    ifeq ($(UNAME_S),Darwin)
        SHARED = cffi.dylib
    else
        SHARED = cffi.so
    endif
endif

.PHONY: all test clean cffi.dll

all: $(SHARED) test_cffi

# ---------------------------------------------------------------------------
# Linux / macOS shared library
# ---------------------------------------------------------------------------
cffi.so: cffi_module.c cffi_types.h
	$(CC) $(CFLAGS) -shared cffi_module.c -o $@ $(LDLIBS)

cffi.dylib: cffi_module.c cffi_types.h
	$(CC) $(CFLAGS) -dynamiclib -undefined dynamic_lookup \
	    cffi_module.c -o $@ $(LDLIBS)

# ---------------------------------------------------------------------------
# Windows DLL (cross-compile from Linux).
#
# libffi is linked statically so the resulting cffi.dll has no
# runtime dependency on libffi-N.dll -- matching the "no extra DLLs"
# property the other modules maintain (see ci.yml's "Verify no MinGW
# runtime DLL deps" step).
# ---------------------------------------------------------------------------
cffi.dll: cffi_module.c cffi_types.h
	$(MINGW_CC) -std=c11 -Wall -Wextra -DCANDO_PLATFORM_WINDOWS \
	    -D_WIN32_WINNT=0x0600 \
	    $(INCLUDES) \
	    -shared cffi_module.c -o $@ \
	    -L$(ROOT) -lcando \
	    -Wl,-Bstatic -lffi -Wl,-Bdynamic

# ---------------------------------------------------------------------------
# C unit tests for the module's pure-C helpers + a libffi smoke.
# These do NOT link against libcando -- only libffi.
# ---------------------------------------------------------------------------
test_cffi: test_cffi.c cffi_types.h
	$(CC) $(CFLAGS) -DCFFI_MODULE_TEST_BUILD \
	    test_cffi.c -o $@ -lffi

test: test_cffi
	./test_cffi

clean:
	rm -f cffi.so cffi.dylib cffi.dll cffi.lib test_cffi
