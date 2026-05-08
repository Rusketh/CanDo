/*
 * lib/jit.h -- script-visible JIT controls.
 *
 * Registers a global `jit` object:
 *
 *   jit.on()              -- enable profiling counters
 *   jit.off()             -- disable profiling counters
 *   jit.toggle()          -- flip current state, returns the new state ("on"/"off")
 *   jit.status()          -- "on" or "off"
 *   jit.isAvailable()     -- TRUE if libcando was built with JIT support
 *                            (today: always TRUE for Phase 2 plumbing,
 *                            even though no machine code is emitted)
 *   jit.stats()           -- {backedges, func_entries, iter_next}
 *   jit.reset()           -- zero all counters
 *
 * Phase 4+ (per docs/jit-plan.md) extends this with traces / mcode
 * accessors; the surface here is forward-compatible.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_LIB_JIT_H
#define CANDO_LIB_JIT_H

#include "../vm/vm.h"

CANDO_API void cando_lib_jit_register(CandoVM *vm);

#endif /* CANDO_LIB_JIT_H */
