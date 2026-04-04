/*
 * lib/process.c -- Subprocess management standard library for Cando.
 *
 * Must compile with gcc -std=c11.
 */

#include "process.h"
#include "libutil.h"
#include "../vm/bridge.h"
#include "../object/object.h"

#include <unistd.h>
#include <sys/types.h>

static int process_pid(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)getpid()));
    return 1;
}

static int process_ppid(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    cando_vm_push(vm, cando_number((f64)getppid()));
    return 1;
}

void cando_lib_process_register(CandoVM *vm)
{
    CandoValue proc_val = cando_bridge_new_object(vm);
    CdoObject *proc_obj = cando_bridge_resolve(vm, proc_val.as.handle);

    libutil_set_method(vm, proc_obj, "pid",  process_pid);
    libutil_set_method(vm, proc_obj, "ppid", process_ppid);

    cando_vm_set_global(vm, "process", proc_val, true);
}
