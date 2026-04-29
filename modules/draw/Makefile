# modules/draw/Makefile -- CanDo draw binary module.
#
# Loaded by scripts as:
#
#     VAR draw = include("./draw.so");          # Linux / macOS
#     VAR draw = include("./draw.dll");         # Windows
#
# This is the skeleton chunk: only `cando_module_init` and a tiny
# namespace object are wired up.  stb_image / stb_truetype vendoring +
# the GL primitives + image / font instances arrive in later chunks.
#
# Targets:
#   all          draw.so + test_draw (default)
#   draw.so      POSIX shared library
#   draw.dll     Windows DLL via mingw-w64
#   test         build and run C unit tests
#   clean        remove artefacts

ROOT      ?= ../..
CC        ?= gcc
MINGW_CC  ?= x86_64-w64-mingw32-gcc

INCLUDES   = -iquote $(ROOT)/source -iquote $(ROOT)/source/core \
             -iquote $(ROOT)/source/parser -iquote $(ROOT)/source/vm \
             -iquote $(ROOT)/source/object -iquote $(ROOT)/source/compat \
             -I$(ROOT)/include -Ivendor

CFLAGS     = -std=c11 -Wall -Wextra -Wpedantic -fPIC $(INCLUDES)

ifeq ($(OS),Windows_NT)
    SHARED   = draw.dll
    LDLIBS   =
else
    UNAME_S := $(shell uname -s)
    LDLIBS   = -lpthread -lm -ldl
    ifeq ($(UNAME_S),Darwin)
        SHARED = draw.dylib
    else
        SHARED = draw.so
    endif
endif

DRAW_OBJS =

.PHONY: all test clean draw.dll

all: $(SHARED) test_draw

# ---------------------------------------------------------------------------
# Linux / macOS shared library
# ---------------------------------------------------------------------------
draw.so: draw_module.c $(DRAW_OBJS)
	$(CC) $(CFLAGS) -shared draw_module.c $(DRAW_OBJS) \
	    -o $@ $(LDLIBS)

draw.dylib: draw_module.c $(DRAW_OBJS)
	$(CC) $(CFLAGS) -dynamiclib -undefined dynamic_lookup \
	    draw_module.c $(DRAW_OBJS) -o $@ $(LDLIBS)

# ---------------------------------------------------------------------------
# Windows DLL (cross-compile from Linux).
# ---------------------------------------------------------------------------
draw.dll: draw_module.c
	$(MINGW_CC) -std=c11 -Wall -Wextra -Wpedantic -DCANDO_PLATFORM_WINDOWS \
	    -D_WIN32_WINNT=0x0600 \
	    -iquote $(ROOT)/source -iquote $(ROOT)/source/core \
	    -iquote $(ROOT)/source/parser -iquote $(ROOT)/source/vm \
	    -iquote $(ROOT)/source/object -iquote $(ROOT)/source/compat \
	    -I$(ROOT)/include -Ivendor \
	    -shared draw_module.c -o $@ \
	    -L$(ROOT) -lcando

# ---------------------------------------------------------------------------
# C unit tests
# ---------------------------------------------------------------------------
test_draw: test_draw.c
	$(CC) $(CFLAGS) -DDRAW_MODULE_TEST_BUILD \
	    test_draw.c -o $@ $(LDLIBS)

test: test_draw
	./test_draw

clean:
	rm -f draw.so draw.dylib draw.dll draw.lib test_draw
