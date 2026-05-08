/*
 * lib/gc.c -- script-visible garbage collector controls.
 *
 * See gc.h for the surface.
 */

#include "gc.h"
#include "libutil.h"
#include "../vm/bridge.h"
#include "../core/memory.h"

/* gc.collect() -- run a full mark-and-sweep cycle on the active VM
 * and return the number of objects swept.                            */
static int gc_collect_native(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    u32 swept = cando_vm_gc_collect(vm);
    cando_vm_push(vm, cando_number((f64)swept));
    return 1;
}

/* gc.count() -- number of currently tracked live heap objects.       */
static int gc_count_native(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    u32 n = (vm->mem) ? vm->mem->live_count : 0;
    cando_vm_push(vm, cando_number((f64)n));
    return 1;
}

/* gc.threshold([n]) -- get or set the auto-collect threshold.
 *   gc.threshold()        returns the current threshold
 *   gc.threshold(0)       disables automatic collection (manual only)
 *   gc.threshold(n>0)     sets the next-collect trigger to n live objs
 *
 * Always returns the new threshold.                                   */
static int gc_threshold_native(CandoVM *vm, int argc, CandoValue *args)
{
    if (vm->mem && argc >= 1 && cando_is_number(args[0])) {
        f64 v = cando_as_number(args[0]);
        u32 n = (v < 0) ? 0 : (u32)v;
        vm->mem->next_collect_threshold = n;
    }
    u32 t = (vm->mem) ? vm->mem->next_collect_threshold : 0;
    cando_vm_push(vm, cando_number((f64)t));
    return 1;
}

void cando_lib_gc_register(CandoVM *vm)
{
    CandoValue val = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(val));

    libutil_set_method(vm, obj, "collect",   gc_collect_native);
    libutil_set_method(vm, obj, "count",     gc_count_native);
    libutil_set_method(vm, obj, "threshold", gc_threshold_native);

    cando_vm_set_global(vm, "gc", val, true);
}
