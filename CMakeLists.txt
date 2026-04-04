cmake_minimum_required(VERSION 3.16)
project(cando C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler flags
if(MSVC)
    add_compile_options(/W3 /D_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -D_GNU_SOURCE)
endif()

# Platform link libraries
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
)

# ----------------------------------------------------------------------------
# cando executable
# ----------------------------------------------------------------------------

add_executable(cando
    ${CORE_SRCS}
    ${OBJECT_SRCS}
    ${PARSER_SRCS}
    ${VM_SRCS}
    ${LIB_SRCS}
    source/natives.c
    source/main.c
)

target_include_directories(cando PRIVATE
    source/core
    source/object
    source/parser
    source/vm
    source
)

target_link_libraries(cando PRIVATE Threads::Threads ${PLATFORM_LIBS})

# ----------------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------------

enable_testing()

# test_core
add_executable(test_core ${CORE_SRCS} tests/test_core.c)
target_include_directories(test_core PRIVATE source/core)
target_link_libraries(test_core PRIVATE Threads::Threads ${PLATFORM_LIBS})
add_test(NAME test_core COMMAND $<TARGET_FILE:test_core>)

# test_object
add_executable(test_object ${CORE_SRCS} ${OBJECT_SRCS} tests/test_object.c)
target_include_directories(test_object PRIVATE source/core source/object)
target_link_libraries(test_object PRIVATE Threads::Threads ${PLATFORM_LIBS})
add_test(NAME test_object COMMAND $<TARGET_FILE:test_object>)

# test_thread
add_executable(test_thread ${CORE_SRCS} ${OBJECT_SRCS} tests/test_thread.c)
target_include_directories(test_thread PRIVATE source/core source/object)
target_link_libraries(test_thread PRIVATE Threads::Threads ${PLATFORM_LIBS})
add_test(NAME test_thread COMMAND $<TARGET_FILE:test_thread>)

# test_lexer
add_executable(test_lexer ${CORE_SRCS} source/parser/lexer.c tests/test_lexer.c)
target_include_directories(test_lexer PRIVATE source/core source/parser source/vm)
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
target_include_directories(test_parser PRIVATE source/core source/parser source/vm)
target_link_libraries(test_parser PRIVATE Threads::Threads ${PLATFORM_LIBS})
add_test(NAME test_parser COMMAND $<TARGET_FILE:test_parser>)

# test_vm
add_executable(test_vm
    ${CORE_SRCS}
    ${OBJECT_SRCS}
    ${VM_SRCS}
    tests/test_vm.c
)
target_include_directories(test_vm PRIVATE source/core source/vm source/object)
target_link_libraries(test_vm PRIVATE Threads::Threads ${PLATFORM_LIBS})
add_test(NAME test_vm COMMAND $<TARGET_FILE:test_vm>)
