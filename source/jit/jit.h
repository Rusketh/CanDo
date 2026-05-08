/*
 * jit/jit.h -- internal JIT module header.
 *
 * Phase 3.1 just exposes the IR data plumbing (see ir.h).  Phase 3.2+
 * extends this header with the recorder API, snapshot type, and trace
 * lifecycle entry points.  Phase 6+ adds the codegen and mcode arena.
 *
 * Public C API (cando_jit_enable, cando_jit_get_stats, ...) lives in
 * source/vm/vm.h alongside the rest of the embedding API; jit.h is for
 * internal users (the dispatch loop, the recorder, future codegen).
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_JIT_JIT_H
#define CANDO_JIT_JIT_H

#include "ir.h"

/* Phase 3.2+ will add CandoTrace, CandoSnapshot, and the recorder
 * entry points here.  For now Phase 3.1 just publishes the IR
 * surface to anyone who #includes this header. */

#endif /* CANDO_JIT_JIT_H */
