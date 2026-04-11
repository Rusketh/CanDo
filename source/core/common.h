/*
 * common.h -- Platform macros, integer typedefs, debug assertions,
 *             and the central allocator interface for Cando.
 *
 * All other Cando headers #include this file first.
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_COMMON_H
#define CANDO_COMMON_H

/* Enable POSIX extensions (strdup, sched_yield, etc.) before any includes */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

/* -----------------------------------------------------------------------
 * C standard includes
 * --------------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdatomic.h>

/* -----------------------------------------------------------------------
 * Compiler / platform detection
 * --------------------------------------------------------------------- */
#if defined(_WIN32) || defined(_WIN64)
#  define CANDO_PLATFORM_WINDOWS 1
#elif defined(__linux__)
#  define CANDO_PLATFORM_LINUX 1
#elif defined(__APPLE__)
#  define CANDO_PLATFORM_MACOS 1
#else
#  define CANDO_PLATFORM_UNKNOWN 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define CANDO_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define CANDO_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define CANDO_NORETURN    __attribute__((noreturn))
#  define CANDO_INLINE      __attribute__((always_inline)) static inline
#else
#  define CANDO_LIKELY(x)   (x)
#  define CANDO_UNLIKELY(x) (x)
#  define CANDO_NORETURN
#  define CANDO_INLINE      static inline
#endif

/* -----------------------------------------------------------------------
 * Symbol visibility / DLL import-export  (CANDO_API)
 *
 * Defined early so all subsequent declarations in this header and in every
 * header that includes common.h can use it.
 *
 * When building libcando itself  : define CANDO_BUILDING_LIB
 * When linking a shared DLL/so   : define CANDO_SHARED
 * When linking the static library: define neither
 * --------------------------------------------------------------------- */
#if defined(CANDO_PLATFORM_WINDOWS)
#  ifdef CANDO_BUILDING_LIB
#    define CANDO_API __declspec(dllexport)
#  elif defined(CANDO_SHARED)
#    define CANDO_API __declspec(dllimport)
#  else
#    define CANDO_API
#  endif
#else
#  ifdef CANDO_BUILDING_LIB
#    define CANDO_API __attribute__((visibility("default")))
#  else
#    define CANDO_API
#  endif
#endif

/* -----------------------------------------------------------------------
 * Fixed-width integer typedefs (convenience aliases)
 * --------------------------------------------------------------------- */
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    i8;
typedef int16_t   i16;
typedef int32_t   i32;
typedef int64_t   i64;
typedef float     f32;
typedef double    f64;
typedef uintptr_t uptr;
typedef size_t    usize;

/* -----------------------------------------------------------------------
 * Debug assertions
 * --------------------------------------------------------------------- */
#ifdef NDEBUG
#  define CANDO_ASSERT(cond)        ((void)0)
#  define CANDO_ASSERT_MSG(cond, msg) ((void)0)
#else
#  define CANDO_ASSERT(cond) \
    do { \
        if (CANDO_UNLIKELY(!(cond))) { \
            fprintf(stderr, "ASSERTION FAILED: %s\n  at %s:%d in %s\n", \
                    #cond, __FILE__, __LINE__, __func__); \
            abort(); \
        } \
    } while (0)
#  define CANDO_ASSERT_MSG(cond, msg) \
    do { \
        if (CANDO_UNLIKELY(!(cond))) { \
            fprintf(stderr, "ASSERTION FAILED: %s -- %s\n  at %s:%d in %s\n", \
                    #cond, (msg), __FILE__, __LINE__, __func__); \
            abort(); \
        } \
    } while (0)
#endif

/* CANDO_UNREACHABLE -- marks code paths the compiler can assume never run */
#define CANDO_UNREACHABLE() \
    do { CANDO_ASSERT_MSG(0, "unreachable"); __builtin_unreachable(); } while (0)

/* -----------------------------------------------------------------------
 * Central allocator interface
 *
 * All Cando heap allocations go through these functions so that a custom
 * allocator or instrumentation can be plugged in at a single point.
 * --------------------------------------------------------------------- */

/*
 * cando_alloc -- allocate `size` bytes.  Aborts on OOM.
 */
CANDO_API void *cando_alloc(usize size);

/*
 * cando_realloc -- resize a previous allocation.  Aborts on OOM.
 */
CANDO_API void *cando_realloc(void *ptr, usize new_size);

/*
 * cando_free -- release memory previously obtained via cando_alloc /
 *              cando_realloc.  Safe to call with NULL.
 */
CANDO_API void cando_free(void *ptr);

/* -----------------------------------------------------------------------
 * Utility macros
 * --------------------------------------------------------------------- */
#define CANDO_ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))
#define CANDO_UNUSED(x)      ((void)(x))
#define CANDO_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

#endif /* CANDO_COMMON_H */
