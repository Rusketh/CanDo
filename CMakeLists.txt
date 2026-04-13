cmake_minimum_required(VERSION 3.16)
project(cando VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ----------------------------------------------------------------------------
# Compiler flags
# ----------------------------------------------------------------------------
if(MSVC)
    add_compile_options(/W3 /D_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -D_GNU_SOURCE)
endif()

# ----------------------------------------------------------------------------
# Platform link libraries
# ----------------------------------------------------------------------------
if(WIN32)
    set(PLATFORM_LIBS ws2_32 m)
else()
    set(PLATFORM_LIBS m dl)
endif()

# Threading
find_package(Threads REQUIRED)

# ----------------------------------------------------------------------------
# Source groups
# ----------------------------------------------------------------------------

set(CORE_SRCS
    source/core/common.c
    source/core/value.c
    source/core/lock.c
    source/core/handle.c
    source/core/memory.c
    source/core/thread_platform.c
)

set(OBJECT_SRCS
    source/object/string.c
    source/object/value.c
    source/object/object.c
    source/object/array.c
    source/object/function.c
    source/object/class.c
    source/object/thread.c
)

set(PARSER_SRCS
    source/parser/lexer.c
    source/parser/parser.c
)

set(VM_SRCS
    source/vm/opcodes.c
    source/vm/chunk.c
    source/vm/bridge.c
    source/vm/vm.c
    source/vm/debug.c
)

set(LIB_SRCS
    source/lib/math.c
    source/lib/file.c
    source/lib/eval.c
    source/lib/string.c
    source/lib/libutil.c
    source/lib/include.c
    source/lib/json.c
    source/lib/csv.c
    source/lib/thread.c
    source/lib/os.c
    source/lib/datetime.c
    source/lib/array.c
    source/lib/crypto.c
    source/lib/process.c
    source/lib/net.c
    source/lib/object.c
)

# Windows compatibility shim (POSIX regex replacement)
if(WIN32)
    list(APPEND LIB_SRCS source/compat/win_regex.c)
endif()

# All library source files (everything except main.c)
set(CANDO_LIB_SRCS
    ${CORE_SRCS}
    ${OBJECT_SRCS}
    ${PARSER_SRCS}
    ${VM_SRCS}
    ${LIB_SRCS}
    source/natives.c
    source/cando_lib.c
)

# ----------------------------------------------------------------------------
# Include path helper
#
# The source tree uses files named "string.h" in source/object/ which would
# shadow the system <string.h> if added via plain -I.  The Makefile avoids
# this with -iquote (only searched for "..." includes, not <...> includes).
# We replicate that here for GCC/Clang.  MSVC doesn't have this problem.
# ----------------------------------------------------------------------------
if(NOT MSVC)
    # Use SHELL: to keep each -iquote + path as a single two-word option.
    # -iquote is searched only for "..." includes, preventing source/object/string.h
    # from shadowing the system <string.h>.  This mirrors the Makefile's -iquote usage.
    set(CANDO_IQUOTE_FLAGS
        "SHELL:-iquote ${CMAKE_SOURCE_DIR}/source"
        "SHELL:-iquote ${CMAKE_SOURCE_DIR}/source/core"
        "SHELL:-iquote ${CMAKE_SOURCE_DIR}/source/object"
        "SHELL:-iquote ${CMAKE_SOURCE_DIR}/source/parser"
        "SHELL:-iquote ${CMAKE_SOURCE_DIR}/source/vm"
        "SHELL:-iquote ${CMAKE_SOURCE_DIR}/source/compat"
    )
else()
    set(CANDO_IQUOTE_FLAGS
        "/I${CMAKE_SOURCE_DIR}/source"
        "/I${CMAKE_SOURCE_DIR}/source/core"
        "/I${CMAKE_SOURCE_DIR}/source/object"
        "/I${CMAKE_SOURCE_DIR}/source/parser"
        "/I${CMAKE_SOURCE_DIR}/source/vm"
    )
endif()

# ----------------------------------------------------------------------------
# Shared library: libcando.so / libcando.dll
# ----------------------------------------------------------------------------
add_library(libcando SHARED ${CANDO_LIB_SRCS})

set_target_properties(libcando PROPERTIES
    OUTPUT_NAME   "cando"
    VERSION       "${PROJECT_VERSION}"
    SOVERSION     "${PROJECT_VERSION_MAJOR}"
)

target_compile_definitions(libcando PRIVATE CANDO_BUILDING_LIB)
target_compile_options(libcando PRIVATE ${CANDO_IQUOTE_FLAGS})

# Public include path: consumers get include/ via the installed interface
target_include_directories(libcando
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/source>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(libcando PUBLIC Threads::Threads ${PLATFORM_LIBS})

if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_link_options(libcando PRIVATE -static-libgcc)
endif()

# ----------------------------------------------------------------------------
# Static library: libcando.a
# ----------------------------------------------------------------------------
add_library(libcando_static STATIC ${CANDO_LIB_SRCS})

set_target_properties(libcando_static PROPERTIES
    OUTPUT_NAME "cando"
)

target_compile_definitions(libcando_static PRIVATE CANDO_BUILDING_LIB)
target_compile_options(libcando_static PRIVATE ${CANDO_IQUOTE_FLAGS})

target_include_directories(libcando_static
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/source>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(libcando_static PUBLIC Threads::Threads ${PLATFORM_LIBS})

# ----------------------------------------------------------------------------
# Executable: cando / cando.exe
# ----------------------------------------------------------------------------

# Windows: embed icon via resource file
if(WIN32)
    set(ICON_RC source/icon.rc)
else()
    set(ICON_RC "")
endif()

add_executable(cando source/main.c ${ICON_RC})

target_compile_options(cando PRIVATE
    "SHELL:-iquote ${CMAKE_SOURCE_DIR}/source"
    "SHELL:-iquote ${CMAKE_SOURCE_DIR}/source/core"
)
target_include_directories(cando PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/source
)

target_link_libraries(cando PRIVATE libcando)

if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_link_options(cando PRIVATE -static-libgcc)
endif()

# ----------------------------------------------------------------------------
# Install targets
# ----------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS libcando libcando_static cando
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES include/cando.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Linux: install .desktop file and icon for system integration
if(UNIX AND NOT APPLE)
    if(EXISTS "${CMAKE_SOURCE_DIR}/assets/icon.png")
        install(FILES assets/icon.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
            RENAME cando.png
        )
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/assets/cando.desktop")
        install(FILES assets/cando.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
        )
    endif()
endif()

# ----------------------------------------------------------------------------
# Tests (compile sources directly — no dependency on libcando)
# ----------------------------------------------------------------------------

enable_testing()

# test_core
add_executable(test_core ${CORE_SRCS} tests/test_core.c)
target_compile_options(test_core PRIVATE -iquote ${CMAKE_SOURCE_DIR}/source/core)
target_link_libraries(test_core PRIVATE Threads::Threads ${PLATFORM_LIBS})
add_test(NAME test_core COMMAND $<TARGET_FILE:test_core>)

# test_object
add_executable(test_object ${CORE_SRCS} ${OBJECT_SRCS} tests/test_object.c)
target_compile_options(test_object PRIVATE ${CANDO_IQUOTE_FLAGS})
target_link_libraries(test_object PRIVATE Threads::Threads ${PLATFORM_LIBS})
add_test(NAME test_object COMMAND $<TARGET_FILE:test_object>)

# test_thread
add_executable(test_thread ${CORE_SRCS} ${OBJECT_SRCS} tests/test_thread.c)
target_compile_options(test_thread PRIVATE ${CANDO_IQUOTE_FLAGS})
target_link_libraries(test_thread PRIVATE Threads::Threads ${PLATFORM_LIBS})
add_test(NAME test_thread COMMAND $<TARGET_FILE:test_thread>)

# test_lexer
add_executable(test_lexer ${CORE_SRCS} source/parser/lexer.c tests/test_lexer.c)
target_compile_options(test_lexer PRIVATE ${CANDO_IQUOTE_FLAGS})
target_link_libraries(test_lexer PRIVATE Threads::Threads ${PLATFORM_LIBS})
add_test(NAME test_lexer COMMAND $<TARGET_FILE:test_lexer>)

# test_parser
add_executable(test_parser
    ${CORE_SRCS}
    source/parser/lexer.c
    source/parser/parser.c
    source/vm/opcodes.c
    source/vm/chunk.c
    tests/test_parser.c
)
target_compile_options(test_parser PRIVATE ${CANDO_IQUOTE_FLAGS})
target_link_libraries(test_parser PRIVATE Threads::Threads ${PLATFORM_LIBS})
add_test(NAME test_parser COMMAND $<TARGET_FILE:test_parser>)

# test_vm
add_executable(test_vm
    ${CORE_SRCS}
    ${OBJECT_SRCS}
    ${VM_SRCS}
    tests/test_vm.c
)
target_compile_options(test_vm PRIVATE ${CANDO_IQUOTE_FLAGS})
target_link_libraries(test_vm PRIVATE Threads::Threads ${PLATFORM_LIBS})
add_test(NAME test_vm COMMAND $<TARGET_FILE:test_vm>)
