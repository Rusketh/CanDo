# modules/forms/Makefile -- CanDo .NET-Forms-shaped Win32 binary module.
#
# Loaded by scripts as:
#
#     VAR forms = include("./forms.dll");        # Windows
#     VAR forms = include("./forms.so");         # Linux  (loads, but every
#                                                #  constructor errors:
#                                                #  the module is Windows-only)
#
# Targets:
#   all          forms.so + test_forms (default)
#   forms.so     POSIX shared library (stub backend)
#   forms.dll    Windows DLL via mingw-w64
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
             -I$(ROOT)/include

CFLAGS     = -std=c11 -Wall -Wextra -Wpedantic -fPIC $(INCLUDES)

# Pure-C support TUs split out under src/core/.  See REWRITE_PLAN.md.
# Each is free-standing (no Win32, no libcando) so it compiles
# identically for the .so / .dll / test builds.
CORE_C     = src/core/color.c        src/core/geom.c   src/core/events.c \
             src/core/slots.c        src/core/dispatch.c \
             src/core/textconv.c \
             src/controls/ctl_common.c \
             src/controls/ctl_checkbox.c \
             src/controls/ctl_progress.c \
             src/controls/ctl_trackbar.c \
             src/controls/ctl_numeric.c \
             src/controls/ctl_textbox.c \
             src/controls/ctl_listbox.c
CORE_H     = src/core/color.h        src/core/geom.h   src/core/events.h \
             src/core/sync.h         src/core/cando_compat.h \
             src/core/slots.h        src/core/manager.h \
             src/core/dispatch.h     src/core/textconv.h \
             src/controls/ctl_common.h \
             src/controls/ctl_checkbox.h \
             src/controls/ctl_progress.h \
             src/controls/ctl_trackbar.h \
             src/controls/ctl_numeric.h \
             src/controls/ctl_textbox.h \
             src/controls/ctl_listbox.h

ifeq ($(OS),Windows_NT)
    SHARED      = forms.dll
    LDLIBS      = -luser32 -lgdi32 -lcomctl32 -lcomdlg32 -lole32 -luuid
else
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
        SHARED  = forms.dylib
        LDLIBS  = -lpthread -lm -ldl
    else
        SHARED  = forms.so
        LDLIBS  = -lpthread -lm -ldl
    endif
endif

.PHONY: all test clean forms.dll

.DEFAULT_GOAL := all

all: $(SHARED) test_forms

# ---------------------------------------------------------------------------
# Linux / macOS shared library (stub backend -- the public surface is
# present but every native errors with "forms is Windows-only" so scripts
# can do a feature check without crashing.)
# ---------------------------------------------------------------------------
forms.so: forms_module.c $(CORE_C) $(CORE_H)
	$(CC) $(CFLAGS) -shared forms_module.c $(CORE_C) -o $@ $(LDLIBS)

forms.dylib: forms_module.c $(CORE_C) $(CORE_H)
	$(CC) $(CFLAGS) -dynamiclib -undefined dynamic_lookup \
	    forms_module.c $(CORE_C) -o $@ $(LDLIBS)

# ---------------------------------------------------------------------------
# Windows DLL (cross-compile from Linux).  Real Win32 backend.
# ---------------------------------------------------------------------------
forms.dll: forms_module.c $(CORE_C) $(CORE_H)
	$(MINGW_CC) -std=c11 -Wall -Wextra -Wpedantic -DCANDO_PLATFORM_WINDOWS \
	    -D_WIN32_WINNT=0x0600 -DUNICODE -D_UNICODE \
	    -iquote $(ROOT)/source -iquote $(ROOT)/source/core \
	    -iquote $(ROOT)/source/parser -iquote $(ROOT)/source/vm \
	    -iquote $(ROOT)/source/object -iquote $(ROOT)/source/compat \
	    -I$(ROOT)/include \
	    -static-libgcc \
	    -shared forms_module.c $(CORE_C) -o $@ \
	    -L$(ROOT) -lcando \
	    -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive \
	    -Wl,-Bdynamic -luser32 -lgdi32 -lcomctl32 -lcomdlg32 -lole32 -luuid \
	    -Wl,-Bstatic

# ---------------------------------------------------------------------------
# C unit tests -- pure-C helpers (event ring buffer, slot allocator,
# property name dispatcher).  Compile the C tests with no Win32 deps so
# they run on Linux CI even though the runtime backend is Windows-only.
# ---------------------------------------------------------------------------
test_forms: test_forms.c forms_module.c $(CORE_C) $(CORE_H)
	$(CC) $(CFLAGS) -DFORMS_MODULE_TEST_BUILD \
	    test_forms.c $(CORE_C) -o $@ $(LDLIBS)

test: test_forms
	./test_forms

clean:
	rm -f forms.so forms.dylib forms.dll forms.lib test_forms
	rm -f *.o
