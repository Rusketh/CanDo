/*
 * vm.h -- Cando stack-based virtual machine.
 *
 * Architecture overview
 * ─────────────────────
 *  CandoVM holds a value stack, a call-frame stack, a try/catch handler
 *  stack, and a loop-depth stack.  All Cando execution happens through
 *  cando_vm_exec(), which dispatches bytecode instructions in a tight loop.
 *
 *  Multi-value returns
 *  ───────────────────
 *  Functions return A values by leaving them on top of the value stack
 *  (OP_RETURN A).  The caller finds exactly A values above its frame base.
 *
 *  Break / continue with depth
 *  ───────────────────────────
 *  Each loop registers a CandoLoopFrame (break target, continue target).
 *  OP_BREAK/OP_CONTINUE walk the loop stack A levels up and jump.
 *
 *  Try / catch / finally unwinding
 *  ────────────────────────────────
 *  OP_TRY_BEGIN pushes a CandoTryFrame recording the catch-block IP and
 *  the value-stack and frame-stack depths at the time of the TRY.  If an
 *  error is raised the VM restores these depths and jumps to the catch IP.
 *  OP_FINALLY_BEGIN records a further jump for the finally block.
 *
 *  Closures and upvalues
 *  ─────────────────────
 *  CandoClosure owns a CandoChunk* (shared; chunks are not owned by the
 *  closure).  Open CandoUpvalue structs point into the value stack;
 *  OP_CLOSE_UPVAL captures the value onto the heap.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_VM_H
#define CANDO_VM_H

#include "../core/common.h"
#include "../core/value.h"
#include "../core/handle.h"
#include "../core/memory.h"
#include "chunk.h"
#include "opcodes.h"

/* =========================================================================
 * Limits
 * ===================================================================== */
#define CANDO_STACK_MAX        2048   /* maximum depth of the value stack   */
#define CANDO_FRAMES_MAX       256    /* maximum call depth                 */
#define CANDO_TRY_MAX          64     /* maximum nested try blocks          */
#define CANDO_LOOP_MAX         64     /* maximum nested loop depth          */
#define CANDO_NATIVE_MAX      128     /* maximum registered native functions */
#define CANDO_MAX_THROW_ARGS   32     /* maximum values in one THROW        */

/* Forward declaration — CandoClosure is defined below. */
typedef struct CandoClosure CandoClosure;

/* =========================================================================
 * Module cache
 *
 * include() resolves every path to an absolute canonical path (via realpath)
 * and stores the module's exported value here the first time it is loaded.
 * Subsequent calls with the same resolved path return the cached value
 * immediately — identical to Node.js require() semantics.
 *
 * For binary (.so/.dylib/.dll) modules the dlopen handle is kept alive so
 * that the loaded library is not unloaded while the VM is running.
 * ===================================================================== */
typedef struct CandoModuleEntry {
    char         *path;      /* heap-allocated NUL-terminated absolute path  */
    CandoValue   *values;    /* cached module exports (retained)             */
    u32           value_count;
    void         *dl_handle; /* dlopen handle, or NULL for script modules    */
    CandoClosure *closure;   /* module closure kept alive for OBJ_FUNCTION   */
    CandoChunk   *chunk;     /* compiled chunk kept alive while closure lives */
} CandoModuleEntry;

/* =========================================================================
 * Native functions
 *
 * Native functions are registered with cando_vm_register_native() and
 * stored in a flat table inside CandoVM.  Each native is exposed as a
 * global variable whose value is a negative number sentinel:
 *   native #0  → -1.0,  native #1 → -2.0, …  (index = -(v) - 1)
 * OP_CALL detects these sentinels and dispatches directly.
 *
 * Stack-return convention (Value Stacking):
 *   Natives push 0 or more return values onto vm->stack via cando_vm_push()
 *   and return the count.  Returning -1 signals an error; vm->has_error and
 *   vm->error_msg must be set before returning -1.
 * ===================================================================== */

/* Forward declaration so CandoNativeFn can reference CandoVM*. */
typedef struct CandoVM CandoVM;

typedef int (*CandoNativeFn)(CandoVM *vm, int argc, CandoValue *args);

/* IS_NATIVE_FN -- true when a value is a native-function sentinel.
 * NATIVE_INDEX -- extract 0-based index from a native-function sentinel. */
#define IS_NATIVE_FN(v)  (cando_is_number(v) && (v).as.number < 0.0)
#define NATIVE_INDEX(v)  ((u32)(-(v).as.number - 1.0))

/* =========================================================================
 * CandoUpvalue -- a captured variable.
 *
 * While the enclosing function is still on the call stack `location`
 * points into the value stack.  When the function returns (OP_CLOSE_UPVAL)
 * the value is copied into `closed` and `location` is redirected to
 * `&closed`.
 *
 * The `lock` field provides thread-safe access when the upvalue is shared
 * between a parent thread and one or more child threads.  OP_LOAD_UPVAL
 * acquires a read lock and OP_STORE_UPVAL acquires a write lock.
 * ===================================================================== */
typedef struct CandoUpvalue {
    CandoLockHeader    lock;      /* read/write lock for cross-thread access */
    CandoValue        *location;  /* current storage of the captured value   */
    CandoValue         closed;    /* heap copy after stack frame is gone     */
    struct CandoUpvalue *next;    /* intrusive linked list of open upvalues  */
} CandoUpvalue;

/* =========================================================================
 * CandoClosure -- a function value: chunk + captured upvalues.
 * ===================================================================== */
typedef struct CandoClosure {
    CandoChunk    *chunk;
    CandoUpvalue **upvalues;    /* array of CandoUpvalue* */
    u32            upvalue_count;
} CandoClosure;

/* =========================================================================
 * CandoCallFrame -- one activation record on the call stack.
 * ===================================================================== */
typedef struct CandoCallFrame {
    CandoClosure *closure;   /* function being executed                   */
    u8           *ip;        /* instruction pointer into closure->chunk   */
    CandoValue   *slots;     /* base of this frame's window in vm->stack  */
    u32           ret_count; /* expected return-value count (0 = any)     */
    bool           is_fluent; /* return receiver instead of result         */
} CandoCallFrame;

/* =========================================================================
 * CandoTryFrame -- one entry in the try/catch handler stack.
 * ===================================================================== */
typedef struct CandoTryFrame {
    u8         *catch_ip;     /* jump here on exception (NULL if none)    */
    u8         *finally_ip;   /* jump here for finally (NULL if none)     */
    u32         stack_save;   /* value-stack depth to restore on unwind   */
    u32         frame_save;   /* call-frame depth to restore on unwind    */
    u32         loop_save;    /* loop-stack depth to restore on unwind    */
} CandoTryFrame;

/* =========================================================================
 * CandoLoopFrame -- one entry in the loop-depth stack.
 * ===================================================================== */
typedef struct CandoLoopFrame {
    u8 *break_ip;    /* target for OP_BREAK   (end of loop)              */
    u8 *cont_ip;     /* target for OP_CONTINUE (top of loop body)        */
} CandoLoopFrame;

/* =========================================================================
 * CandoGlobalEntry -- one slot in the flat global hash table.
 * ===================================================================== */
typedef struct CandoGlobalEntry {
    CandoString *key;       /* interned key string; NULL = empty slot     */
    CandoValue   value;
    bool         is_const;  /* true = write-protected after definition    */
} CandoGlobalEntry;

/* =========================================================================
 * CandoGlobalEnv -- open-addressed hash table for global variables.
 *
 * The `lock` provides thread-safe access when globals are shared between
 * a parent VM and child thread VMs.  OP_LOAD_GLOBAL acquires a read lock
 * and OP_STORE_GLOBAL / OP_DEF_GLOBAL acquire a write lock.
 * ===================================================================== */
typedef struct CandoGlobalEnv {
    CandoLockHeader   lock;      /* read/write lock for cross-thread access */
    CandoGlobalEntry *entries;
    u32               capacity;  /* always a power of two                 */
    u32               count;
} CandoGlobalEnv;

/* =========================================================================
 * CandoVM -- the interpreter state.
 * ===================================================================== */
typedef enum {
    VM_OK,          /* execution completed normally                       */
    VM_RUNTIME_ERR, /* unhandled runtime error                            */
    VM_HALT,        /* OP_HALT reached                                    */
    VM_EVAL_DONE,   /* eval chunk returned (internal use by exec_eval)    */
} CandoVMResult;

struct CandoVM {
    /* Value stack ------------------------------------------------------ */
    CandoValue  stack[CANDO_STACK_MAX];
    CandoValue *stack_top;   /* one past the last pushed value            */

    /* Call frames ------------------------------------------------------ */
    CandoCallFrame frames[CANDO_FRAMES_MAX];
    u32            frame_count;

    /* Try/catch stack -------------------------------------------------- */
    CandoTryFrame  try_stack[CANDO_TRY_MAX];
    u32            try_depth;

    /* Loop stack ------------------------------------------------------- */
    CandoLoopFrame loop_stack[CANDO_LOOP_MAX];
    u32            loop_depth;

    /* Open upvalue list ------------------------------------------------ */
    CandoUpvalue  *open_upvalues;  /* linked, sorted by stack slot        */

    /* Globals ---------------------------------------------------------- */
    /* Pointer to the active global environment.  For the root VM this
     * points to globals_owned (allocated by cando_vm_init).  For child
     * thread VMs it points to the parent's globals so global variable
     * accesses are shared.                                               */
    CandoGlobalEnv  *globals;
    CandoGlobalEnv  *globals_owned; /* heap-allocated env; NULL for child VMs */

    /* Native functions ------------------------------------------------- */
    CandoNativeFn  native_fns[CANDO_NATIVE_MAX];
    u32            native_count;

    /* Memory controller (may be NULL for unit tests) ------------------- */
    CandoMemCtrl  *mem;

    /* Handle table: maps HandleIndex -> CdoObject* -------------------- */
    /* For the root VM this points to handles_owned (allocated by init).
     * Child thread VMs share the parent's handle table via this pointer. */
    CandoHandleTable  *handles;
    CandoHandleTable  *handles_owned; /* heap-allocated table; NULL for children */

    /* Error state ------------------------------------------------------ */
    CandoValue     error_vals[CANDO_MAX_THROW_ARGS]; /* thrown values     */
    u32            error_val_count;                  /* how many were thrown */
    bool           has_error;
    char           error_msg[512];

    /* Multi-return spreading ------------------------------------------- */
    int            last_ret_count; /* actual return count from last call  */
    int            spread_extra;   /* accumulated extra args from spreading */

    /* Eval re-entrancy ------------------------------------------------- */
    u32            eval_stop_frame;   /* frame_count at which OP_RETURN stops */
    CandoValue    *eval_results;      /* captured results when VM_EVAL_DONE   */
    u32            eval_result_count;
    u32            eval_result_cap;

    /* Thread result capture (used by cando_vm_exec_closure) ----------- */
    /* When thread_stop_frame is non-zero, OP_RETURN at that frame depth
     * captures ALL return values here instead of returning them to the
     * caller frame.  Used so the thread trampoline can collect results. */
    u32            thread_stop_frame;
    CandoValue     thread_results[CANDO_MAX_THROW_ARGS]; /* reuse limit    */
    u32            thread_result_count;

    /* Built-in type prototypes ----------------------------------------- */
    CandoValue     string_proto;    /* string method table (or null)        */
    CandoValue     array_proto;     /* array method table (or null)         */

    /* Module cache (used by include()) --------------------------------- */
    CandoModuleEntry *module_cache;       /* heap array of cached entries   */
    u32               module_cache_count; /* number of entries used         */
    u32               module_cache_cap;   /* allocated capacity             */
};

/* =========================================================================
 * VM lifecycle
 * ===================================================================== */

/* cando_vm_init -- initialise all fields; mem may be NULL. */
void cando_vm_init(CandoVM *vm, CandoMemCtrl *mem);

/*
 * cando_vm_init_child -- initialise a child VM for use in a spawned thread.
 *
 * The child shares the parent's handle table and global environment (both
 * via pointer — no new heap allocation for those).  Native functions are
 * copied by value.  The child owns its own stack and call frames.
 *
 * The caller must eventually call cando_vm_destroy() on the child.
 */
void cando_vm_init_child(CandoVM *child, const CandoVM *parent);

/* cando_vm_destroy -- release all VM-owned resources. */
void cando_vm_destroy(CandoVM *vm);

/* =========================================================================
 * Closure helpers
 * ===================================================================== */

/* cando_closure_new -- allocate a closure around a chunk. */
CandoClosure *cando_closure_new(CandoChunk *chunk);

/* cando_closure_free -- release a closure (does NOT free the chunk). */
void cando_closure_free(CandoClosure *closure);

/* =========================================================================
 * Execution
 * ===================================================================== */

/*
 * cando_vm_exec -- execute a top-level chunk from the beginning.
 *
 * Wraps the chunk in a closure, pushes the first call frame, and runs
 * the dispatch loop until OP_HALT, OP_RETURN at frame 0, or an unhandled
 * error.
 *
 * Returns VM_OK, VM_HALT, or VM_RUNTIME_ERR.  On VM_RUNTIME_ERR the
 * error description is in vm->error_msg.
 */
CandoVMResult cando_vm_exec(CandoVM *vm, CandoChunk *chunk);

/*
 * cando_vm_exec_eval -- execute a chunk compiled in eval mode (eval_mode=true).
 *
 * Runs the chunk in a fresh call frame inside the currently running VM.
 * Safe to call from within a native function (re-entrant).
 *
 * On VM_EVAL_DONE: *result_out receives the expression value (or null).
 * On VM_RUNTIME_ERR: vm->error_msg is set; *result_out is unchanged.
 * The chunk must outlive this call; the caller is responsible for freeing it.
 */
CandoVMResult cando_vm_exec_eval(CandoVM *vm, CandoChunk *chunk,
                                  CandoValue **results_out, u32 *count_out);

/*
 * cando_vm_exec_closure -- execute a script closure on `vm` starting at
 * `fn_pc` within `closure->chunk`, passing zero arguments.
 *
 * Return values are captured in vm->thread_results[0..thread_result_count-1].
 * Used by the thread trampoline to run a thread body and collect its results.
 *
 * Returns VM_OK/VM_HALT on success, VM_RUNTIME_ERR on error.
 */
CandoVMResult cando_vm_exec_closure(CandoVM *vm, CandoClosure *closure,
                                     u32 fn_pc);

/*
 * cando_vm_exec_eval_module -- like cando_vm_exec_eval but transfers
 * ownership of the module closure to the caller instead of freeing it.
 *
 * Used by include() to keep module closures alive so that OBJ_FUNCTION
 * values created by OP_CLOSURE inside the module remain callable after
 * the module finishes executing.
 *
 * On success: *closure_out receives the closure (caller must eventually
 * call cando_closure_free, typically via CandoModuleEntry in vm_destroy).
 * On error: *closure_out is set to NULL.
 */
CandoVMResult cando_vm_exec_eval_module(CandoVM *vm, CandoChunk *chunk,
                                         CandoValue **results_out, u32 *count_out,
                                         CandoClosure **closure_out);

/* =========================================================================
 * Stack helpers (useful for native function bindings and tests)
 * ===================================================================== */

/*
 * cando_vm_error -- report an error from native function code.
 *
 * Formats the message into vm->error_msg, sets vm->has_error, and
 * populates vm->error_vals[0] with the message string so the error is
 * catchable by a TRY/CATCH block.
 *
 * Call this instead of setting vm->has_error and vm->error_msg directly.
 * After calling this, return -1 from your CandoNativeFn.
 */
void cando_vm_error(CandoVM *vm, const char *fmt, ...);

/* cando_vm_push -- push a value; aborts on stack overflow. */
void cando_vm_push(CandoVM *vm, CandoValue val);

/* cando_vm_pop -- pop and return the top value; aborts on underflow. */
CandoValue cando_vm_pop(CandoVM *vm);

/* cando_vm_peek -- return the value at distance `dist` from the top
 * without removing it (0 = top). */
CandoValue cando_vm_peek(const CandoVM *vm, u32 dist);

/* cando_vm_stack_depth -- number of values currently on the stack. */
u32 cando_vm_stack_depth(const CandoVM *vm);

/* =========================================================================
 * Native function API
 * ===================================================================== */

/*
 * cando_vm_register_native -- register a C function as a native callable.
 *
 * Assigns the next available sentinel value (-(count+1)) to the function,
 * stores it in vm->native_fns[], and defines a global variable `name` with
 * that sentinel so scripts can call it by name.
 *
 * Returns true on success, false if CANDO_NATIVE_MAX is exceeded.
 */
bool cando_vm_register_native(CandoVM *vm, const char *name,
                               CandoNativeFn fn);

/*
 * cando_vm_add_native -- register a native without exposing it as a global.
 * Returns the sentinel CandoValue that represents this function.
 * Returns cando_null() if CANDO_NATIVE_MAX is exceeded.
 */
CandoValue cando_vm_add_native(CandoVM *vm, CandoNativeFn fn);

/* =========================================================================
 * Global variable API
 * ===================================================================== */

/* cando_vm_set_global -- define or overwrite a global variable.
 * If `is_const` is true the variable is write-protected after this call. */
bool cando_vm_set_global(CandoVM *vm, const char *name, CandoValue val,
                          bool is_const);

/* cando_vm_get_global -- look up a global; returns false if not found. */
bool cando_vm_get_global(const CandoVM *vm, const char *name,
                          CandoValue *out);

/* =========================================================================
 * Meta-method dispatch helper
 * ===================================================================== */

/*
 * cando_vm_call_meta -- look up meta_key on the object at handle h and invoke
 * the meta-method with the given args (CandoValue[] of length argc).
 *
 * On success: pushes one result value onto vm->stack and returns true.
 * On missing meta-method: does nothing, returns false, vm->has_error unchanged.
 * On call error: sets vm->has_error and returns false.
 *
 * Supports: native-function sentinels (IS_NATIVE_FN) and OBJ_NATIVE CdoObjects.
 * Script-function meta-methods require closure support (not yet implemented).
 *
 * meta_key is a CdoString* (object-layer interned string pointer).
 */
struct CdoString;  /* forward declaration; full type is in object/string.h */
bool cando_vm_call_meta(CandoVM *vm, HandleIndex h,
                         struct CdoString *meta_key,
                         CandoValue *args, u32 argc);

/* =========================================================================
 * Threading helpers
 * ===================================================================== */

/*
 * cando_current_thread -- return the CdoThread* for the currently executing
 * Cando thread, or NULL when called from the main (non-spawned) thread.
 *
 * Implemented via thread-local storage; valid from any OS thread.
 */
struct CdoThread; /* forward — full type in object/thread.h */
struct CdoThread *cando_current_thread(void);

/*
 * cando_vm_call_value -- call a Cando function value with argc arguments.
 *
 * fn_val must be an OBJ_FUNCTION handle.  args[0..argc-1] are the arguments
 * (extra args beyond the function's arity are ignored; missing args are null).
 * Return values are pushed onto vm->stack; the count is returned.
 *
 * Safe to call from within a native function (re-entrant via vm_run).
 * Returns 0 if fn_val is not callable or the call stack overflows.
 */
int cando_vm_call_value(CandoVM *vm, CandoValue fn_val,
                         CandoValue *args, u32 argc);

#endif /* CANDO_VM_H */
