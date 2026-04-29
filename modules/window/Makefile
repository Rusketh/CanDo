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
             -I$(ROOT)/include -Ivendor -Ivendor/glfw/include

CFLAGS     = -std=c11 -Wall -Wextra -Wpedantic -fPIC $(INCLUDES)

# ---------------------------------------------------------------------------
# Vendored GLFW 3.4
#
# We build GLFW from source statically into window.so / window.dll so the
# user's system never needs libglfw at runtime.  The X11 backend is selected
# on Linux (-D_GLFW_X11) and the Win32 backend on Windows (-D_GLFW_WIN32);
# everything else (Wayland, Cocoa) is left out.
#
# GLFW's source uses C99 designated initialisers, comments, and other forms
# our -Wpedantic config flags as warnings; silence those for the upstream
# tree only -- our own module .c files keep the strict flags.
# ---------------------------------------------------------------------------
GLFW_DIR     = vendor/glfw/src
GLFW_INCLUDE = -Ivendor/glfw/include -I$(GLFW_DIR)

GLFW_QUIET = -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers \
             -Wno-pedantic -Wno-cast-function-type -Wno-format

# Common GLFW translation units (always built).
GLFW_COMMON_SRC = \
    $(GLFW_DIR)/init.c       $(GLFW_DIR)/context.c    $(GLFW_DIR)/window.c   \
    $(GLFW_DIR)/input.c      $(GLFW_DIR)/monitor.c    $(GLFW_DIR)/platform.c \
    $(GLFW_DIR)/vulkan.c                                                     \
    $(GLFW_DIR)/null_init.c  $(GLFW_DIR)/null_joystick.c                     \
    $(GLFW_DIR)/null_monitor.c $(GLFW_DIR)/null_window.c                     \
    $(GLFW_DIR)/egl_context.c $(GLFW_DIR)/osmesa_context.c

# Linux / X11 backend.
GLFW_X11_SRC = \
    $(GLFW_DIR)/x11_init.c   $(GLFW_DIR)/x11_monitor.c $(GLFW_DIR)/x11_window.c \
    $(GLFW_DIR)/xkb_unicode.c $(GLFW_DIR)/glx_context.c                          \
    $(GLFW_DIR)/posix_module.c $(GLFW_DIR)/posix_poll.c                          \
    $(GLFW_DIR)/posix_thread.c $(GLFW_DIR)/posix_time.c                          \
    $(GLFW_DIR)/linux_joystick.c

# Windows / Win32 backend.
GLFW_WIN32_SRC = \
    $(GLFW_DIR)/win32_init.c    $(GLFW_DIR)/win32_joystick.c \
    $(GLFW_DIR)/win32_module.c  $(GLFW_DIR)/win32_monitor.c  \
    $(GLFW_DIR)/win32_thread.c  $(GLFW_DIR)/win32_time.c     \
    $(GLFW_DIR)/win32_window.c  $(GLFW_DIR)/wgl_context.c

ifeq ($(OS),Windows_NT)
    SHARED      = window.dll
    LDLIBS      = -lopengl32 -lgdi32 -luser32 -lshell32
    GLFW_DEFS   = -D_GLFW_WIN32 -DUNICODE -D_UNICODE
    GLFW_SRC    = $(GLFW_COMMON_SRC) $(GLFW_WIN32_SRC)
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
        # macOS support is intentionally not vendored yet -- placeholder.
        SHARED    = window.dylib
        LDLIBS    = -lpthread -lm -ldl
        GLFW_DEFS = -D_GLFW_X11
        GLFW_SRC  = $(GLFW_COMMON_SRC) $(GLFW_X11_SRC)
    else
        SHARED    = window.so
        LDLIBS    = -lpthread -lm -ldl -lX11 -lXcursor -lXrandr -lXinerama -lXi -lGL -lGLX -lrt
        GLFW_DEFS = -D_GLFW_X11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_GNU_SOURCE
        GLFW_SRC  = $(GLFW_COMMON_SRC) $(GLFW_X11_SRC)
    endif
endif

# Build GLFW translation units once into a static archive so both the
# .so and the (future) C unit-test binary can reuse them without recompiling.
GLFW_OBJS = $(GLFW_SRC:.c=.o)

%.o: %.c
	$(CC) -std=c99 -fPIC -O2 $(GLFW_DEFS) $(GLFW_QUIET) \
	    -Ivendor/glfw/include -I$(GLFW_DIR) -c $< -o $@

vendor/glfw/libglfw.a: $(GLFW_OBJS)
	ar rcs $@ $(GLFW_OBJS)

WINDOW_OBJS = vendor/glfw/libglfw.a

.PHONY: all test clean window.dll

.DEFAULT_GOAL := all

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
# Windows DLL (cross-compile from Linux).  The MinGW toolchain compiles GLFW
# with -D_GLFW_WIN32 inline, so no system glfw is required at runtime.
# ---------------------------------------------------------------------------
window.dll: window_module.c $(GLFW_SRC)
	$(MINGW_CC) -std=c11 -Wall -Wextra -Wpedantic -DCANDO_PLATFORM_WINDOWS \
	    -D_WIN32_WINNT=0x0600 \
	    -D_GLFW_WIN32 -DUNICODE -D_UNICODE \
	    -iquote $(ROOT)/source -iquote $(ROOT)/source/core \
	    -iquote $(ROOT)/source/parser -iquote $(ROOT)/source/vm \
	    -iquote $(ROOT)/source/object -iquote $(ROOT)/source/compat \
	    -I$(ROOT)/include -Ivendor -Ivendor/glfw/include -I$(GLFW_DIR) \
	    -shared window_module.c $(GLFW_COMMON_SRC) $(GLFW_WIN32_SRC) -o $@ \
	    -L$(ROOT) -lcando -lopengl32 -lgdi32 -luser32 -lshell32 \
	    $(GLFW_QUIET)

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
	rm -f $(GLFW_OBJS) vendor/glfw/libglfw.a
