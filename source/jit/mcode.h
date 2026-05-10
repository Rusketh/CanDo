/*
 * jit/mcode.h -- executable machine-code buffer abstraction.
 *
 * Phase 6.0 of docs/jit-plan.md.  The codegen path needs pages with
 * PROT_READ|PROT_EXEC so a trace's emitted x86_64 bytes can be
 * jumped to from C.  This module wraps mmap/mprotect/munmap behind
 * a small API the codegen can call without learning the platform's
 * page-protection vocabulary.
 *
 * Lifecycle: alloc() -> write bytes -> finalize() -> call.  After
 * finalize the buffer is rwx-flipped to r-x and the function pointer
 * inside is safe to invoke.  free() releases the mapping back to
 * the kernel.
 *
 * Linux-only in v1.  The plan calls out Windows/macOS in Phase 8;
 * those need VirtualAlloc / mmap+pthread_jit_write_protect_np
 * respectively.  An #ifdef _WIN32 / __APPLE__ branch will live here
 * when those land.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_JIT_MCODE_H
#define CANDO_JIT_MCODE_H

#include "../core/common.h"

/* CandoMCode owns one page-aligned executable mapping. */
typedef struct CandoMCode {
    u8 *base;     /* start of the mapping (NULL if unallocated)        */
    u32 size;     /* bytes allocated -- always a multiple of page size */
    u32 written;  /* bytes the codegen has written so far              */
    u8  finalized;/* 1 once cando_mcode_finalize has been called       */
} CandoMCode;

/* Allocate `bytes` of writable memory, rounded up to the next page.
 * Returns true on success; on failure base stays NULL and the caller
 * should fall back to the IR-interpreter.  bytes==0 returns true with
 * a NULL mapping (caller checks base before writing). */
CANDO_API bool cando_mcode_alloc(CandoMCode *m, u32 bytes);

/* Append `len` bytes to the buffer.  Returns false if out of space. */
CANDO_API bool cando_mcode_write(CandoMCode *m, const void *bytes, u32 len);

/* Flip the mapping from rw- to r-x so its contents can be executed.
 * Idempotent (subsequent calls are no-ops).  Returns false on
 * mprotect failure -- the caller should treat that as a codegen
 * failure and discard the buffer. */
CANDO_API bool cando_mcode_finalize(CandoMCode *m);

/* Release the mapping.  Idempotent. */
CANDO_API void cando_mcode_free(CandoMCode *m);

#endif /* CANDO_JIT_MCODE_H */
