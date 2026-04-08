/*
 * eval.c -- Native eval(code [, options]) function for Cando.
 *
 * eval(code)              -- compile `code` as Cando, run it, return the result
 *                            of the last expression (or null for statements).
 * eval(code, options)     -- same, with an options table:
 *   options.name   (string) -- name used in error messages / stack traces
 *                              (default: "<eval>")
 *   options.sandbox (bool)  -- if true, run in an isolated global environment:
 *                              outer user-defined globals are hidden and any VAR
 *                              declarations made inside are discarded on return.
 *                              Native functions remain accessible.
 *                              (default: false)
 *
 * Re-entrancy: safe to call while the VM is executing; uses
 * cando_vm_exec_eval() which runs a nested vm_run() call.
 */

#include "eval.h"
#include "../parser/parser.h"
#include "../vm/chunk.h"
#include "../vm/bridge.h"
#include "../object/object.h"
#include "../object/string.h"
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * sandbox_enter / sandbox_exit
 *
 * Temporarily replaces vm->globals with a fresh environment containing only
 * native-function sentinel entries so that built-in functions remain callable
 * but user-defined globals are invisible.  Any VAR declarations made while the
 * sandbox is active are discarded by sandbox_exit.
 * ---------------------------------------------------------------------- */
static void sandbox_enter(CandoVM *vm, CandoGlobalEnv **saved_out)
{
    /* Save the current globals pointer so sandbox_exit can restore it. */
    *saved_out = vm->globals;

    CandoGlobalEnv *sandbox = (CandoGlobalEnv *)cando_alloc(sizeof(CandoGlobalEnv));
    cando_lock_init(&sandbox->lock);
    sandbox->capacity = 64;
    sandbox->count    = 0;
    sandbox->entries  = (CandoGlobalEntry *)cando_alloc(
                            64 * sizeof(CandoGlobalEntry));
    memset(sandbox->entries, 0, 64 * sizeof(CandoGlobalEntry));
    vm->globals = sandbox;

    /* Copy only native-function sentinels (IS_NATIVE_FN: negative numbers). */
    CandoGlobalEnv *parent = *saved_out;
    for (u32 i = 0; i < parent->capacity; i++) {
        CandoGlobalEntry *e = &parent->entries[i];
        if (!e->key || !IS_NATIVE_FN(e->value)) continue;
        cando_vm_set_global(vm, e->key->data, e->value, e->is_const);
    }
}

static void sandbox_exit(CandoVM *vm, CandoGlobalEnv **saved)
{
    /* Release all keys and values created inside the sandbox. */
    CandoGlobalEnv *sandbox = vm->globals;
    for (u32 i = 0; i < sandbox->capacity; i++) {
        CandoGlobalEntry *e = &sandbox->entries[i];
        if (e->key) {
            cando_string_release(e->key);
            cando_value_release(e->value);
        }
    }
    cando_free(sandbox->entries);
    cando_free(sandbox);

    /* Restore the caller's global environment. */
    vm->globals = *saved;
}

/* -------------------------------------------------------------------------
 * native_eval
 * ---------------------------------------------------------------------- */
static int native_eval(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "eval: first argument must be a string");
        return -1;
    }

    CandoString *src = args[0].as.string;

    /* --- Parse options from an optional second argument (object) --- */
    char        name_buf[256];
    const char *chunk_name = "<eval>";
    bool        sandbox    = false;

    if (argc >= 2 && cando_is_object(args[1])) {
        CdoObject *opts = cando_bridge_resolve(vm, args[1].as.handle);

        /* options.name: string → chunk name for error messages */
        CdoString *name_key = cdo_string_intern("name", 4);
        CdoValue   name_field;
        if (cdo_object_rawget(opts, name_key, &name_field)
                && name_field.tag == CDO_STRING) {
            u32 len = name_field.as.string->length;
            if (len > 255) len = 255;
            memcpy(name_buf, name_field.as.string->data, len);
            name_buf[len] = '\0';
            chunk_name = name_buf;
        }
        cdo_string_release(name_key);

        /* options.sandbox: bool → isolate global environment */
        CdoString *sb_key = cdo_string_intern("sandbox", 7);
        CdoValue   sb_field;
        if (cdo_object_rawget(opts, sb_key, &sb_field)
                && sb_field.tag == CDO_BOOL) {
            sandbox = sb_field.as.boolean;
        }
        cdo_string_release(sb_key);
    }

    /* --- Compile the source string in eval mode --- */
    CandoChunk  *chunk = cando_chunk_new(chunk_name, 0, false);
    CandoParser  parser;
    cando_parser_init(&parser, src->data, src->length, chunk);
    parser.eval_mode = true;

    if (!cando_parse(&parser)) {
        cando_vm_error(vm, "eval parse error: %s", cando_parser_error(&parser));
        cando_chunk_free(chunk);
        return -1;
    }

    /* --- Execute, optionally inside a sandboxed global environment --- */
    CandoGlobalEnv *saved_globals = NULL;
    if (sandbox) sandbox_enter(vm, &saved_globals);

    CandoValue   *results = NULL;
    u32           result_count = 0;
    CandoVMResult res = cando_vm_exec_eval(vm, chunk, &results, &result_count);
    cando_chunk_free(chunk);

    if (sandbox) sandbox_exit(vm, &saved_globals);

    if (res == VM_RUNTIME_ERR) {
        /* vm->has_error and vm->error_msg are already set by the VM. */
        return -1;
    }

    if (result_count == 0) {
        cando_free(results); /* NULL-safe; may have been allocated for a bare RETURN; */
        cando_vm_push(vm, cando_null());
        return 1;
    }

    for (u32 i = 0; i < result_count; i++)
        cando_vm_push(vm, results[i]); /* transfer ownership to stack */
    cando_free(results);               /* free the array wrapper, not the values */
    return (int)result_count;
}

void cando_lib_eval_register(CandoVM *vm)
{
    cando_vm_register_native(vm, "eval", native_eval);
}
