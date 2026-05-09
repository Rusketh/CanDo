/*
 * lib/jit.c -- script-visible JIT controls.  See jit.h for the surface.
 *
 * Phase 2 of docs/jit-plan.md: this only manages profiling counters.
 * The recorder, optimiser, and codegen land in Phase 3+; this module
 * is forward-compatible with the surface those phases need.
 */

#include "jit.h"
#include "libutil.h"
#include "../vm/bridge.h"

/* jit.on() -- enable JIT profiling counters.  Returns "on". */
static int jit_on_native(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    cando_jit_enable(vm);
    libutil_push_cstr(vm, "on");
    return 1;
}

/* jit.off() -- disable JIT profiling counters.  Returns "off". */
static int jit_off_native(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    cando_jit_disable(vm);
    libutil_push_cstr(vm, "off");
    return 1;
}

/* jit.toggle() -- flip the current state and return the new one. */
static int jit_toggle_native(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    if (cando_jit_is_enabled(vm)) {
        cando_jit_disable(vm);
        libutil_push_cstr(vm, "off");
    } else {
        cando_jit_enable(vm);
        libutil_push_cstr(vm, "on");
    }
    return 1;
}

/* jit.status() -- "on" or "off". */
static int jit_status_native(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    libutil_push_cstr(vm, cando_jit_is_enabled(vm) ? "on" : "off");
    return 1;
}

/* jit.isAvailable() -- TRUE if libcando was built with JIT support.
 * Today the answer is always TRUE because the Phase 2 counters ship
 * unconditionally; once the codegen lands behind -DCANDO_ENABLE_JIT
 * (per the plan), this returns the build-flag value. */
static int jit_is_available_native(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    cando_vm_push(vm, cando_bool(true));
    return 1;
}

/* jit.stats() -- returns an object whose keys mirror the C-side stats:
 *   backedges, func_entries, iter_next         (aggregate hit counts)
 *   trace_starts, trace_aborts                 (recorder bookkeeping)
 *   hot_pcs, blacklisted_pcs                   (hot-table state)
 * Field names are stable; new counters can be appended without
 * breaking scripts that read individual keys. */
static int jit_stats_native(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;

    CandoJitStats st  = cando_jit_get_stats(vm);
    CandoValue    obj = cando_bridge_new_object(vm);
    CdoObject    *o   = cando_bridge_resolve(vm, cando_as_handle(obj));

    struct { const char *name; u32 len; f64 value; } fields[] = {
        { "backedges",        9, (f64)st.backedge_hits    },
        { "func_entries",    12, (f64)st.func_entry_hits  },
        { "iter_next",        9, (f64)st.iter_next_hits   },
        { "trace_starts",    12, (f64)st.trace_starts     },
        { "trace_aborts",    12, (f64)st.trace_aborts     },
        { "traces_compiled", 15, (f64)st.traces_compiled  },
        { "hot_pcs",          7, (f64)st.hot_pcs          },
        { "blacklisted_pcs", 15, (f64)st.blacklisted_pcs  },
    };

    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        CdoString *k = cdo_string_intern(fields[i].name, fields[i].len);
        cdo_object_rawset(o, k, cdo_number(fields[i].value), FIELD_NONE);
        cdo_string_release(k);
    }

    cando_vm_push(vm, obj);
    return 1;
}

/* jit.reset() -- zero all counters; returns null. */
static int jit_reset_native(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    cando_jit_reset_stats(vm);
    cando_vm_push(vm, cando_null());
    return 1;
}

void cando_lib_jit_register(CandoVM *vm)
{
    CandoValue val = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(val));

    libutil_set_method(vm, obj, "on",          jit_on_native);
    libutil_set_method(vm, obj, "off",         jit_off_native);
    libutil_set_method(vm, obj, "toggle",      jit_toggle_native);
    libutil_set_method(vm, obj, "status",      jit_status_native);
    libutil_set_method(vm, obj, "isAvailable", jit_is_available_native);
    libutil_set_method(vm, obj, "stats",       jit_stats_native);
    libutil_set_method(vm, obj, "reset",       jit_reset_native);

    cando_vm_set_global(vm, "jit", val, true);
}
