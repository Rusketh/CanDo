/*
 * jit/mcode.c -- executable machine-code buffer.
 *
 * See mcode.h for the surface.  The implementation is a thin wrapper
 * around mmap + mprotect (POSIX) or VirtualAlloc + VirtualProtect
 * (Windows); the buffer starts as PROT_READ|PROT_WRITE so the codegen
 * can populate it, then flips to PROT_READ|PROT_EXEC once finalized.
 */

#include "mcode.h"

#include <string.h>

#if defined(_WIN32)
#  include <windows.h>

static u32 mcode_page_size(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize > 0 ? (u32)si.dwPageSize : 4096u;
}

bool cando_mcode_alloc(CandoMCode *m, u32 bytes) {
    if (!m) return false;
    m->base      = NULL;
    m->size      = 0;
    m->written   = 0;
    m->finalized = 0;
    if (bytes == 0) return true;

    u32 ps      = mcode_page_size();
    u32 rounded = (bytes + ps - 1) & ~(ps - 1);
    void *p = VirtualAlloc(NULL, rounded, MEM_COMMIT | MEM_RESERVE,
                            PAGE_READWRITE);
    if (!p) return false;

    m->base = (u8 *)p;
    m->size = rounded;
    return true;
}

bool cando_mcode_finalize(CandoMCode *m) {
    if (!m || !m->base) return false;
    if (m->finalized) return true;
    DWORD old;
    if (!VirtualProtect(m->base, m->size, PAGE_EXECUTE_READ, &old))
        return false;
    /* x86_64 guarantees i-cache coherency for self-modified code on
     * the same core; AArch64 will need FlushInstructionCache here. */
    m->finalized = 1;
    return true;
}

void cando_mcode_free(CandoMCode *m) {
    if (!m || !m->base) return;
    VirtualFree(m->base, 0, MEM_RELEASE);
    m->base      = NULL;
    m->size      = 0;
    m->written   = 0;
    m->finalized = 0;
}

#else /* POSIX */

#  include <unistd.h>
#  include <sys/mman.h>

static u32 mcode_page_size(void) {
    long sz = sysconf(_SC_PAGESIZE);
    return sz > 0 ? (u32)sz : 4096u;
}

bool cando_mcode_alloc(CandoMCode *m, u32 bytes) {
    if (!m) return false;
    m->base      = NULL;
    m->size      = 0;
    m->written   = 0;
    m->finalized = 0;
    if (bytes == 0) return true;

    u32 ps     = mcode_page_size();
    u32 rounded = (bytes + ps - 1) & ~(ps - 1);
    void *p = mmap(NULL, rounded, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return false;

    m->base = (u8 *)p;
    m->size = rounded;
    return true;
}

bool cando_mcode_finalize(CandoMCode *m) {
    if (!m || !m->base) return false;
    if (m->finalized) return true;
    if (mprotect(m->base, m->size, PROT_READ | PROT_EXEC) != 0)
        return false;
    /* The Intel architecture guarantees instruction-cache coherency
     * for self-modified code on the same core, so no explicit
     * __builtin___clear_cache is needed for x86_64.  AArch64 will
     * need it when the plan extends to Phase 9. */
    m->finalized = 1;
    return true;
}

void cando_mcode_free(CandoMCode *m) {
    if (!m || !m->base) return;
    munmap(m->base, m->size);
    m->base      = NULL;
    m->size      = 0;
    m->written   = 0;
    m->finalized = 0;
}

#endif /* _WIN32 vs POSIX */

bool cando_mcode_write(CandoMCode *m, const void *bytes, u32 len) {
    if (!m || !m->base) return false;
    if (m->finalized) return false;          /* immutable post-finalize */
    if (m->written + len > m->size) return false;
    memcpy(m->base + m->written, bytes, len);
    m->written += len;
    return true;
}
