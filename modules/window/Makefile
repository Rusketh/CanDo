# modules/window/Makefile -- CanDo window binary module.
#
# Loaded by scripts as:
#
#     VAR window = include("./window.so");      # Linux / macOS
#     VAR window = include("./window.dll");     # Windows
#
# This is the skeleton chunk: only `cando_module_init` and a tiny
# namespace object are wired up.  GLFW vendoring + window creation +
# the per-window render thread land in subsequent chunks.
#
# Targets:
#   all          window.so + test_window (default)
#   window.so    POSIX shared library
#   window.dll   Windows DLL via mingw-w64
#   test         build and run C unit tests
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
             -I$(ROOT)/include -Ivendor

CFLAGS     = -std=c11 -Wall -Wextra -Wpedantic -fPIC $(INCLUDES)

ifeq ($(OS),Windows_NT)
    SHARED   = window.dll
    LDLIBS   =
else
    UNAME_S := $(shell uname -s)
    LDLIBS   = -lpthread -lm -ldl
    ifeq ($(UNAME_S),Darwin)
        SHARED = window.dylib
    else
        SHARED = window.so
    endif
endif

WINDOW_OBJS =

.PHONY: all test clean window.dll

all: $(SHARED) test_window

# ---------------------------------------------------------------------------
# Linux / macOS shared library
# ---------------------------------------------------------------------------
window.so: window_module.c $(WINDOW_OBJS)
	$(CC) $(CFLAGS) -shared window_module.c $(WINDOW_OBJS) \
	    -o $@ $(LDLIBS)

window.dylib: window_module.c $(WINDOW_OBJS)
	$(CC) $(CFLAGS) -dynamiclib -undefined dynamic_lookup \
	    window_module.c $(WINDOW_OBJS) -o $@ $(LDLIBS)

# ---------------------------------------------------------------------------
# Windows DLL (cross-compile from Linux).
# ---------------------------------------------------------------------------
window.dll: window_module.c
	$(MINGW_CC) -std=c11 -Wall -Wextra -Wpedantic -DCANDO_PLATFORM_WINDOWS \
	    -D_WIN32_WINNT=0x0600 \
	    -iquote $(ROOT)/source -iquote $(ROOT)/source/core \
	    -iquote $(ROOT)/source/parser -iquote $(ROOT)/source/vm \
	    -iquote $(ROOT)/source/object -iquote $(ROOT)/source/compat \
	    -I$(ROOT)/include -Ivendor \
	    -shared window_module.c -o $@ \
	    -L$(ROOT) -lcando

# ---------------------------------------------------------------------------
# C unit tests
# ---------------------------------------------------------------------------
test_window: test_window.c
	$(CC) $(CFLAGS) -DWINDOW_MODULE_TEST_BUILD \
	    test_window.c -o $@ $(LDLIBS)

test: test_window
	./test_window

clean:
	rm -f window.so window.dylib window.dll window.lib test_window
