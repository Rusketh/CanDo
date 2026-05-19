/*
 * modules/cffi/cffi_module.c -- CanDo cffi binary module.
 *
 * Loaded into a script with:
 *
 *     VAR ffi = include("./cffi.so");        // Linux / macOS
 *     VAR ffi = include("./cffi.dll");       // Windows
 *
 * Provides script-level access to arbitrary C-ABI shared libraries by
 * pairing dlopen / LoadLibrary with libffi.  This first delivery is
 * milestones 1-3 of the PLAN.md roadmap: scalar calls, pointer/buffer
 * lifecycle, NUL-terminated strings.  Structs (milestone 4), callbacks
 * (5), and the `declare()` header parser (6) arrive in follow-ups.
 *
 * Calling conventions:
 *
 *     // direct -- both calling styles work, like every other module:
 *     ffi.call(lib, "fnname", "ret(args)", arg1, arg2, ...)
 *     lib:call("fnname", "ret(args)", arg1, arg2, ...)
 *
 *     // bound -- signature is parsed once at bind time:
 *     VAR fn = lib:bind("fnname", "ret(args)");
 *     fn:call(arg1, arg2, ...);
 *
 * Both routes funnel through native_cffi_call_impl, which is the only
 * place that actually invokes libffi.
 *
 * Must compile with gcc / clang / MinGW-w64 -std=c11.
 */

#include <cando.h>
#include "vm/bridge.h"
#include "object/object.h"
#include "object/array.h"
#include "object/string.h"
#include "object/value.h"
#include "lib/libutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <errno.h>

#include <ffi.h>

/* Portable mutex shim, identical to the one in modules/ldap and
 * modules/sqlite -- libcando's cando_os_mutex_* helpers lack CANDO_API
 * and so cannot be linked from a separately-built shared object on
 * Windows.  Roll our own; the surface used by this module is tiny. */
#if defined(_WIN32) || defined(_WIN64)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
   typedef CRITICAL_SECTION cffi_mutex_t;
#  define CFFI_MUTEX_INIT(m)   InitializeCriticalSection(m)
#  define CFFI_MUTEX_LOCK(m)   EnterCriticalSection(m)
#  define CFFI_MUTEX_UNLOCK(m) LeaveCriticalSection(m)
#  define CFFI_PLATFORM_WINDOWS 1
#else
#  include <dlfcn.h>
#  include <pthread.h>
   typedef pthread_mutex_t cffi_mutex_t;
#  define CFFI_MUTEX_INIT(m)   pthread_mutex_init(m, NULL)
#  define CFFI_MUTEX_LOCK(m)   pthread_mutex_lock(m)
#  define CFFI_MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
#  define CFFI_PLATFORM_POSIX 1
#endif

#include "cffi_types.h"

#define CFFI_MODULE_VERSION "0.1.0"

/* =========================================================================
 * Pools
 *
 * Three resource kinds, each behind its own static pool with a mutex.
 * Handle objects expose a single number field naming the slot index;
 * the natives validate the index on every call before dereferencing.
 * This is the same trick modules/sqlite/sqlite_module.c uses.
 * ======================================================================= */

#define CFFI_MAX_LIBS  64
#define CFFI_MAX_BINDS 1024
#define CFFI_MAX_BUFS  4096

#define CFFI_LIB_SLOT_KEY   "__cffi_lib_slot"
#define CFFI_BIND_SLOT_KEY  "__cffi_bind_slot"
#define CFFI_BUF_SLOT_KEY   "__cffi_buf_slot"

typedef struct LibSlot {
    void *handle;             /* dlopen / LoadLibrary handle */
    bool  in_use;
} LibSlot;

typedef struct BindSlot {
    void   *fn;               /* dlsym result, cast to void* */
    CffiSig sig;
    int     lib_slot;         /* lib this binding belongs to; -1 if lib closed */
    bool    in_use;
} BindSlot;

typedef struct BufSlot {
    void  *data;
    size_t len;               /* 0 when length is unknown (foreign pointers) */
    bool   owned;             /* true => we malloc'd; freed on release */
    bool   in_use;
} BufSlot;

static LibSlot       g_lib_pool[CFFI_MAX_LIBS];
static BindSlot      g_bind_pool[CFFI_MAX_BINDS];
static BufSlot       g_buf_pool[CFFI_MAX_BUFS];

static cffi_mutex_t  g_lib_mutex;
static cffi_mutex_t  g_bind_mutex;
static cffi_mutex_t  g_buf_mutex;

static _Atomic(int)  g_inited = 0;

static void ensure_pools_inited(void)
{
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_inited, &expected, 1)) {
        CFFI_MUTEX_INIT(&g_lib_mutex);
        CFFI_MUTEX_INIT(&g_bind_mutex);
        CFFI_MUTEX_INIT(&g_buf_mutex);
        for (int i = 0; i < CFFI_MAX_LIBS;  i++) g_lib_pool[i].in_use  = false;
        for (int i = 0; i < CFFI_MAX_BINDS; i++) g_bind_pool[i].in_use = false;
        for (int i = 0; i < CFFI_MAX_BUFS;  i++) g_buf_pool[i].in_use  = false;
    }
}

static int alloc_lib_slot(void)
{
    CFFI_MUTEX_LOCK(&g_lib_mutex);
    int slot = -1;
    for (int i = 0; i < CFFI_MAX_LIBS; i++) {
        if (!g_lib_pool[i].in_use) {
            g_lib_pool[i].in_use = true;
            g_lib_pool[i].handle = NULL;
            slot = i;
            break;
        }
    }
    CFFI_MUTEX_UNLOCK(&g_lib_mutex);
    return slot;
}

static int alloc_bind_slot(void)
{
    CFFI_MUTEX_LOCK(&g_bind_mutex);
    int slot = -1;
    for (int i = 0; i < CFFI_MAX_BINDS; i++) {
        if (!g_bind_pool[i].in_use) {
            g_bind_pool[i].in_use   = true;
            g_bind_pool[i].fn       = NULL;
            g_bind_pool[i].lib_slot = -1;
            slot = i;
            break;
        }
    }
    CFFI_MUTEX_UNLOCK(&g_bind_mutex);
    return slot;
}

static int alloc_buf_slot(void)
{
    CFFI_MUTEX_LOCK(&g_buf_mutex);
    int slot = -1;
    for (int i = 0; i < CFFI_MAX_BUFS; i++) {
        if (!g_buf_pool[i].in_use) {
            g_buf_pool[i].in_use = true;
            g_buf_pool[i].data   = NULL;
            g_buf_pool[i].len    = 0;
            g_buf_pool[i].owned  = false;
            slot = i;
            break;
        }
    }
    CFFI_MUTEX_UNLOCK(&g_buf_mutex);
    return slot;
}

/* =========================================================================
 * Object helpers (interning + setters / getters).  Mirrors the helpers
 * in modules/sqlite/sqlite_module.c so the patterns stay consistent
 * across the binary modules.
 * ======================================================================= */

static void obj_set_number(CdoObject *obj, const char *key, f64 value)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    cdo_object_rawset(obj, k, cdo_number(value), FIELD_NONE);
    cdo_string_release(k);
}

static void obj_set_string(CdoObject *obj, const char *key,
                           const char *data, u32 len)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoString *s = cdo_string_intern(data, len);
    cdo_object_rawset(obj, k, cdo_string_value(s), FIELD_NONE);
    cdo_string_release(s);
    cdo_string_release(k);
}

static bool obj_get_number(CdoObject *obj, const char *key, f64 *out)
{
    CdoString *k = cdo_string_intern(key, (u32)strlen(key));
    CdoValue v;
    bool ok = cdo_object_rawget(obj, k, &v);
    cdo_string_release(k);
    if (!ok || v.tag != CDO_NUMBER) return false;
    *out = v.as.number;
    return true;
}

/* =========================================================================
 * Method-sentinel registry (same pattern as modules/sqlite).
 *
 * libutil_set_method registers a native with the VM and returns the
 * sentinel value (a unique negative number).  We cache each sentinel
 * by name + target so freshly-made handles get the same set of
 * methods automatically.
 * ======================================================================= */

typedef enum {
    METHOD_MODULE_ONLY = 0,
    METHOD_ON_LIB      = 1,
    METHOD_ON_BIND     = 2,
    METHOD_ON_BUF      = 3,
} MethodTarget;

typedef struct MethodEntry {
    const char  *name;
    f64          sentinel;
    MethodTarget target;
} MethodEntry;

#define MAX_METHOD_ENTRIES 32
static MethodEntry g_methods[MAX_METHOD_ENTRIES];
static int         g_method_count = 0;

static void register_method(CandoVM *vm, CdoObject *mod_obj,
                            const char *name, CandoNativeFn fn,
                            MethodTarget target)
{
    CandoValue sentinel = cando_vm_add_native(vm, fn);
    f64 s = cando_is_number(sentinel) ? cando_as_number(sentinel) : 0.0;
    obj_set_number(mod_obj, name, s);
    if (g_method_count < MAX_METHOD_ENTRIES) {
        g_methods[g_method_count].name     = name;
        g_methods[g_method_count].sentinel = s;
        g_methods[g_method_count].target   = target;
        g_method_count++;
    }
}

static void attach_methods_for(CdoObject *handle, MethodTarget target)
{
    for (int i = 0; i < g_method_count; i++) {
        if (g_methods[i].target == target) {
            obj_set_number(handle, g_methods[i].name,
                           g_methods[i].sentinel);
        }
    }
}

/* =========================================================================
 * Handle slot lookup
 * ======================================================================= */

static int handle_slot(CandoVM *vm, CandoValue v, const char *key, int max)
{
    if (!cando_is_object(v)) return -1;
    CdoObject *o = cando_bridge_resolve(vm, cando_as_handle(v));
    if (!o) return -1;
    f64 n = -1.0;
    if (!obj_get_number(o, key, &n)) return -1;
    int i = (int)n;
    if (i < 0 || i >= max) return -1;
    return i;
}

static int lib_handle_slot(CandoVM *vm, CandoValue v)
{
    int s = handle_slot(vm, v, CFFI_LIB_SLOT_KEY, CFFI_MAX_LIBS);
    if (s < 0) return -1;
    return g_lib_pool[s].in_use ? s : -1;
}

static int bind_handle_slot(CandoVM *vm, CandoValue v)
{
    int s = handle_slot(vm, v, CFFI_BIND_SLOT_KEY, CFFI_MAX_BINDS);
    if (s < 0) return -1;
    return g_bind_pool[s].in_use ? s : -1;
}

static int buf_handle_slot(CandoVM *vm, CandoValue v)
{
    int s = handle_slot(vm, v, CFFI_BUF_SLOT_KEY, CFFI_MAX_BUFS);
    if (s < 0) return -1;
    return g_buf_pool[s].in_use ? s : -1;
}

/* =========================================================================
 * Handle constructors
 * ======================================================================= */

static CandoValue make_lib_handle(CandoVM *vm, int slot)
{
    CandoValue v   = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(v));
    obj_set_number(obj, CFFI_LIB_SLOT_KEY, (f64)slot);
    attach_methods_for(obj, METHOD_ON_LIB);
    return v;
}

static CandoValue make_bind_handle(CandoVM *vm, int lib_slot, int bind_slot)
{
    CandoValue v   = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(v));
    obj_set_number(obj, CFFI_LIB_SLOT_KEY,  (f64)lib_slot);
    obj_set_number(obj, CFFI_BIND_SLOT_KEY, (f64)bind_slot);
    attach_methods_for(obj, METHOD_ON_BIND);
    return v;
}

static CandoValue make_buf_handle(CandoVM *vm, int slot)
{
    CandoValue v   = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(v));
    obj_set_number(obj, CFFI_BUF_SLOT_KEY, (f64)slot);
    attach_methods_for(obj, METHOD_ON_BUF);
    return v;
}

/* =========================================================================
 * libffi mapping
 * ======================================================================= */

static ffi_type *ffi_type_for_kind(CffiKind k)
{
    switch (k) {
        case CFFI_VOID: return &ffi_type_void;
        case CFFI_BOOL: return &ffi_type_uint8;
        case CFFI_I8:   return &ffi_type_sint8;
        case CFFI_U8:   return &ffi_type_uint8;
        case CFFI_I16:  return &ffi_type_sint16;
        case CFFI_U16:  return &ffi_type_uint16;
        case CFFI_I32:  return &ffi_type_sint32;
        case CFFI_U32:  return &ffi_type_uint32;
        case CFFI_I64:  return &ffi_type_sint64;
        case CFFI_U64:  return &ffi_type_uint64;
        case CFFI_F32:  return &ffi_type_float;
        case CFFI_F64:  return &ffi_type_double;
        case CFFI_PTR:  return &ffi_type_pointer;
        case CFFI_CSTR: return &ffi_type_pointer;
        default:        return &ffi_type_void;
    }
}

/* =========================================================================
 * Argument marshalling
 *
 * Each script argument is copied into a fixed-size storage cell that
 * libffi sees by pointer.  Pointer-kind arguments may also need a
 * malloc'd NUL-terminated copy of a script string when the original
 * isn't already terminated -- though CanDo strings always are, so the
 * fast path passes string->data straight through.
 * ======================================================================= */

typedef union ArgCell {
    int8_t   i8;
    uint8_t  u8;
    int16_t  i16;
    uint16_t u16;
    int32_t  i32;
    uint32_t u32;
    int64_t  i64;
    uint64_t u64;
    float    f32;
    double   f64;
    void    *ptr;
} ArgCell;

/* Marshal one CandoValue into the cell for the given kind.  Returns
 * false (and writes a message to `errbuf`) on type mismatch. */
static bool marshal_arg(CandoVM *vm, CffiKind k, CandoValue v,
                        ArgCell *cell, int idx, char *errbuf, size_t errcap)
{
    (void)vm;
    /* Integers: accept TYPE_NUMBER (incl. boolean as 0/1) and TYPE_BOOL. */
    if (cffi_kind_is_integer(k)) {
        double d;
        if (cando_is_number(v))       d = cando_as_number(v);
        else if (cando_is_bool(v))    d = cando_as_bool(v) ? 1.0 : 0.0;
        else {
            snprintf(errbuf, errcap,
                "argument %d: expected number (%s), got non-numeric value",
                idx + 1, cffi_kind_name(k));
            return false;
        }
        switch (k) {
            case CFFI_BOOL: cell->u8  = (uint8_t)(d != 0.0); return true;
            case CFFI_I8:   cell->i8  = (int8_t)(int64_t)d;  return true;
            case CFFI_U8:   cell->u8  = (uint8_t)(int64_t)d; return true;
            case CFFI_I16:  cell->i16 = (int16_t)(int64_t)d; return true;
            case CFFI_U16:  cell->u16 = (uint16_t)(int64_t)d;return true;
            case CFFI_I32:  cell->i32 = (int32_t)(int64_t)d; return true;
            case CFFI_U32:  cell->u32 = (uint32_t)(int64_t)d;return true;
            case CFFI_I64:  cell->i64 = (int64_t)d;          return true;
            case CFFI_U64:  cell->u64 = (uint64_t)(int64_t)d;return true;
            default: break;
        }
    }

    if (cffi_kind_is_float(k)) {
        if (!cando_is_number(v)) {
            snprintf(errbuf, errcap,
                "argument %d: expected number (%s), got non-numeric value",
                idx + 1, cffi_kind_name(k));
            return false;
        }
        double d = cando_as_number(v);
        if (k == CFFI_F32) cell->f32 = (float)d;
        else               cell->f64 = d;
        return true;
    }

    /* Pointer-shaped argument.  Accepts:
     *   - null           -> NULL
     *   - string         -> NUL-terminated string->data
     *   - buffer handle  -> buf->data
     *   - number         -> raw uintptr_t (escape hatch for opaque ids)
     */
    if (cffi_kind_is_pointer(k)) {
        if (cando_is_null(v)) {
            cell->ptr = NULL;
            return true;
        }
        if (cando_is_string(v)) {
            CandoString *s = cando_as_string(v);
            cell->ptr = (void *)(uintptr_t)s->data;
            return true;
        }
        if (cando_is_object(v)) {
            CdoObject *o = cando_bridge_resolve(vm, cando_as_handle(v));
            if (o) {
                f64 n;
                if (obj_get_number(o, CFFI_BUF_SLOT_KEY, &n)) {
                    int slot = (int)n;
                    if (slot >= 0 && slot < CFFI_MAX_BUFS && g_buf_pool[slot].in_use) {
                        cell->ptr = g_buf_pool[slot].data;
                        return true;
                    }
                    snprintf(errbuf, errcap,
                        "argument %d: buffer was freed", idx + 1);
                    return false;
                }
            }
            snprintf(errbuf, errcap,
                "argument %d: expected pointer, got an unrelated object",
                idx + 1);
            return false;
        }
        if (cando_is_number(v)) {
            cell->ptr = (void *)(uintptr_t)(int64_t)cando_as_number(v);
            return true;
        }
        snprintf(errbuf, errcap,
            "argument %d: expected pointer / string / buffer, got %s",
            idx + 1,
            cando_is_bool(v) ? "bool" : "unknown");
        return false;
    }

    snprintf(errbuf, errcap,
        "argument %d: unsupported kind %s", idx + 1, cffi_kind_name(k));
    return false;
}

/* =========================================================================
 * Return-value demarshalling
 *
 * libffi widens integer returns to ffi_arg.  Sign-extension is the
 * caller's responsibility for signed kinds.  Pointer returns are
 * either wrapped in a new buf handle (foreign / unknown length) or
 * returned as cando_null() when NULL.
 * ======================================================================= */

static CandoValue demarshal_return(CandoVM *vm, CffiKind k, void *rstorage)
{
    switch (k) {
        case CFFI_VOID:
            return cando_null();

        case CFFI_BOOL: {
            ffi_arg w = *(ffi_arg *)rstorage;
            return cando_bool((w & 0xff) != 0);
        }

        case CFFI_I8: {
            ffi_arg w = *(ffi_arg *)rstorage;
            int8_t i = (int8_t)(w & 0xff);
            return cando_number((double)i);
        }
        case CFFI_U8: {
            ffi_arg w = *(ffi_arg *)rstorage;
            return cando_number((double)(uint8_t)(w & 0xff));
        }
        case CFFI_I16: {
            ffi_arg w = *(ffi_arg *)rstorage;
            return cando_number((double)(int16_t)(w & 0xffff));
        }
        case CFFI_U16: {
            ffi_arg w = *(ffi_arg *)rstorage;
            return cando_number((double)(uint16_t)(w & 0xffff));
        }
        case CFFI_I32: {
            ffi_arg w = *(ffi_arg *)rstorage;
            return cando_number((double)(int32_t)(w & 0xffffffff));
        }
        case CFFI_U32: {
            ffi_arg w = *(ffi_arg *)rstorage;
            return cando_number((double)(uint32_t)(w & 0xffffffff));
        }
        case CFFI_I64: {
            int64_t i = *(int64_t *)rstorage;
            return cando_number((double)i);
        }
        case CFFI_U64: {
            uint64_t u = *(uint64_t *)rstorage;
            return cando_number((double)u);
        }
        case CFFI_F32: {
            float f = *(float *)rstorage;
            return cando_number((double)f);
        }
        case CFFI_F64: {
            double d = *(double *)rstorage;
            return cando_number(d);
        }
        case CFFI_PTR:
        case CFFI_CSTR: {
            void *p = *(void **)rstorage;
            if (!p) return cando_null();
            int s = alloc_buf_slot();
            if (s < 0) {
                cando_vm_error(vm, "cffi: buffer pool exhausted");
                return cando_null();
            }
            g_buf_pool[s].data  = p;
            g_buf_pool[s].len   = 0;
            g_buf_pool[s].owned = false;
            return make_buf_handle(vm, s);
        }
        default:
            return cando_null();
    }
}

/* =========================================================================
 * The actual call site.  Takes a parsed signature, function pointer,
 * and the argv window for the script-level arguments.
 *
 * Returns 1 (one return value pushed) on success.  On error, sets the
 * VM error and returns -1.
 * ======================================================================= */

static int do_ffi_call(CandoVM *vm, void *fn, const CffiSig *sig,
                       CandoValue *script_args, int script_argc,
                       const char *call_name)
{
    if (sig->nargs != script_argc) {
        cando_vm_error(vm,
            "%s: signature expects %d argument%s, got %d",
            call_name, sig->nargs, sig->nargs == 1 ? "" : "s", script_argc);
        return -1;
    }

    ffi_cif    cif;
    ffi_type  *atypes[CFFI_MAX_ARGS];
    ArgCell    cells[CFFI_MAX_ARGS];
    void      *argp[CFFI_MAX_ARGS];

    for (int i = 0; i < sig->nargs; i++) {
        atypes[i] = ffi_type_for_kind(sig->args[i]);
        argp[i]   = &cells[i];
    }

    ffi_status st = ffi_prep_cif(&cif, FFI_DEFAULT_ABI,
                                  (unsigned)sig->nargs,
                                  ffi_type_for_kind(sig->ret),
                                  atypes);
    if (st != FFI_OK) {
        cando_vm_error(vm, "%s: ffi_prep_cif failed (%d)", call_name, (int)st);
        return -1;
    }

    char ebuf[160];
    for (int i = 0; i < sig->nargs; i++) {
        ebuf[0] = '\0';
        if (!marshal_arg(vm, sig->args[i], script_args[i], &cells[i],
                         i, ebuf, sizeof(ebuf))) {
            cando_vm_error(vm, "%s: %s", call_name,
                           ebuf[0] ? ebuf : "argument marshal failed");
            return -1;
        }
    }

    /* Return storage must be at least sizeof(ffi_arg) for integer
     * widening; allocate the largest reasonable scalar to cover float /
     * double / pointer paths too. */
    union {
        ffi_arg  pad;
        double   d;
        float    f;
        void    *p;
        uint64_t u64;
        int64_t  i64;
    } rstore;
    memset(&rstore, 0, sizeof(rstore));

    /* Clear errno before the call so ffi.errno() reflects only the C
     * side's behaviour.  This is the same convention libc uses. */
    errno = 0;

    ffi_call(&cif, FFI_FN(fn), &rstore, argp);

    CandoValue ret = demarshal_return(vm, sig->ret, &rstore);
    if (vm && /* error flag set by demarshal? */ 0) return -1;
    cando_vm_push(vm, ret);
    return 1;
}

/* =========================================================================
 * Natives -- ffi.load / lib:close
 * ======================================================================= */

static void *plat_dlopen(const char *path, char *errbuf, size_t errcap)
{
#if CFFI_PLATFORM_WINDOWS
    HMODULE h = LoadLibraryA(path);
    if (!h) {
        DWORD code = GetLastError();
        snprintf(errbuf, errcap, "LoadLibrary failed (0x%lx)",
                 (unsigned long)code);
        return NULL;
    }
    return (void *)h;
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *err = dlerror();
        snprintf(errbuf, errcap, "dlopen failed: %s",
                 err ? err : "(no error message)");
        return NULL;
    }
    return h;
#endif
}

static void *plat_dlsym(void *handle, const char *name)
{
#if CFFI_PLATFORM_WINDOWS
    return (void *)GetProcAddress((HMODULE)handle, name);
#else
    return dlsym(handle, name);
#endif
}

static void plat_dlclose(void *handle)
{
#if CFFI_PLATFORM_WINDOWS
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
}

/* ffi.load(path) -> library handle
 * Also accepts ffi.load() with no args -> current process. */
static int native_cffi_load(CandoVM *vm, int argc, CandoValue *args)
{
    ensure_pools_inited();

    void *handle = NULL;
    char ebuf[256] = {0};

    if (argc == 0 || cando_is_null(args[0])) {
#if CFFI_PLATFORM_WINDOWS
        handle = (void *)GetModuleHandleA(NULL);
#else
        handle = dlopen(NULL, RTLD_NOW | RTLD_LOCAL);
#endif
        if (!handle) {
            cando_vm_error(vm, "ffi.load: failed to resolve current process handle");
            return -1;
        }
    } else if (cando_is_string(args[0])) {
        const char *path = cando_as_string(args[0])->data;
        handle = plat_dlopen(path, ebuf, sizeof(ebuf));
        if (!handle) {
            cando_vm_error(vm, "ffi.load(\"%s\"): %s", path, ebuf);
            return -1;
        }
    } else {
        cando_vm_error(vm, "ffi.load: expected string path or null");
        return -1;
    }

    int slot = alloc_lib_slot();
    if (slot < 0) {
        plat_dlclose(handle);
        cando_vm_error(vm, "ffi.load: library pool exhausted (max %d)",
                       CFFI_MAX_LIBS);
        return -1;
    }
    g_lib_pool[slot].handle = handle;

    cando_vm_push(vm, make_lib_handle(vm, slot));
    return 1;
}

/* ffi.current() -- shorthand for ffi.load(NULL) */
static int native_cffi_current(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc; (void)args;
    CandoValue noarg = cando_null();
    return native_cffi_load(vm, 1, &noarg);
}

/* ffi.close(lib) / lib:close()
 *
 * Releases the dlopen handle, marks the slot free, and best-effort
 * invalidates every binding that was derived from this library.
 * Idempotent.
 */
static int native_cffi_close(CandoVM *vm, int argc, CandoValue *args)
{
    (void)argc;
    int slot = (argc > 0) ? lib_handle_slot(vm, args[0]) : -1;
    if (slot < 0) {
        /* Already closed, or never a real handle -- treat as success. */
        cando_vm_push(vm, cando_null());
        return 1;
    }

    /* Invalidate every binding referencing this library. */
    CFFI_MUTEX_LOCK(&g_bind_mutex);
    for (int i = 0; i < CFFI_MAX_BINDS; i++) {
        if (g_bind_pool[i].in_use && g_bind_pool[i].lib_slot == slot) {
            g_bind_pool[i].fn       = NULL;
            g_bind_pool[i].lib_slot = -1;
            /* Leave in_use true so the binding slot still resolves and
             * we can throw a clean "library was closed" error rather
             * than a generic "invalid binding" later. */
        }
    }
    CFFI_MUTEX_UNLOCK(&g_bind_mutex);

    CFFI_MUTEX_LOCK(&g_lib_mutex);
    void *h = g_lib_pool[slot].handle;
    g_lib_pool[slot].handle = NULL;
    g_lib_pool[slot].in_use = false;
    CFFI_MUTEX_UNLOCK(&g_lib_mutex);

    if (h) plat_dlclose(h);

    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * Natives -- lib:bind / binding:call / lib:call
 * ======================================================================= */

/* Resolve "library handle, name, signature" into a function pointer
 * and a parsed signature.  Used by both `bind` and the direct `call`
 * path. */
static bool resolve_call(CandoVM *vm, CandoValue lib_val,
                         CandoValue name_val, CandoValue sig_val,
                         int *out_lib_slot, void **out_fn, CffiSig *out_sig,
                         const char *fn_name_for_err)
{
    int lib_slot = lib_handle_slot(vm, lib_val);
    if (lib_slot < 0) {
        cando_vm_error(vm, "%s: first argument must be a library handle",
                       fn_name_for_err);
        return false;
    }
    if (!cando_is_string(name_val)) {
        cando_vm_error(vm, "%s: symbol name must be a string", fn_name_for_err);
        return false;
    }
    if (!cando_is_string(sig_val)) {
        cando_vm_error(vm, "%s: signature must be a string", fn_name_for_err);
        return false;
    }
    const char *name = cando_as_string(name_val)->data;
    const char *sig_text = cando_as_string(sig_val)->data;

    CFFI_MUTEX_LOCK(&g_lib_mutex);
    void *handle = g_lib_pool[lib_slot].handle;
    CFFI_MUTEX_UNLOCK(&g_lib_mutex);
    if (!handle) {
        cando_vm_error(vm, "%s: library was closed", fn_name_for_err);
        return false;
    }

    void *sym = plat_dlsym(handle, name);
    if (!sym) {
        cando_vm_error(vm, "%s: symbol \"%s\" not found in library",
                       fn_name_for_err, name);
        return false;
    }

    CffiParseError perr;
    if (!cffi_parse_signature(sig_text, out_sig, &perr)) {
        cando_vm_error(vm, "%s: bad signature \"%s\" at offset %zu: %s",
                       fn_name_for_err, sig_text, perr.offset, perr.message);
        return false;
    }

    *out_lib_slot = lib_slot;
    *out_fn       = sym;
    return true;
}

/* ffi.call(lib, "name", "sig", args...)  or  lib:call("name", "sig", args...)
 * The handle is always args[0]. */
static int native_cffi_call(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 3) {
        cando_vm_error(vm,
            "ffi.call: expected (lib, name, signature, args...)");
        return -1;
    }

    void *fn = NULL;
    int   lib_slot = -1;
    CffiSig sig;
    if (!resolve_call(vm, args[0], args[1], args[2],
                      &lib_slot, &fn, &sig, "ffi.call"))
        return -1;

    return do_ffi_call(vm, fn, &sig, &args[3], argc - 3, "ffi.call");
}

/* lib:bind("name", "sig") -> binding handle */
static int native_cffi_bind(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 3) {
        cando_vm_error(vm,
            "ffi.bind: expected (lib, name, signature)");
        return -1;
    }

    void *fn = NULL;
    int   lib_slot = -1;
    CffiSig sig;
    if (!resolve_call(vm, args[0], args[1], args[2],
                      &lib_slot, &fn, &sig, "ffi.bind"))
        return -1;

    int bind_slot = alloc_bind_slot();
    if (bind_slot < 0) {
        cando_vm_error(vm, "ffi.bind: binding pool exhausted (max %d)",
                       CFFI_MAX_BINDS);
        return -1;
    }
    g_bind_pool[bind_slot].fn       = fn;
    g_bind_pool[bind_slot].sig      = sig;
    g_bind_pool[bind_slot].lib_slot = lib_slot;

    CandoValue h = make_bind_handle(vm, lib_slot, bind_slot);
    /* Carry the name + signature as readable properties so debug /
     * print(inspect()) tells you what the binding is. */
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(h));
    if (obj) {
        const char *name = cando_as_string(args[1])->data;
        u32         nlen = cando_as_string(args[1])->length;
        obj_set_string(obj, "name", name, nlen);
        const char *st = cando_as_string(args[2])->data;
        u32         sl = cando_as_string(args[2])->length;
        obj_set_string(obj, "signature", st, sl);
    }

    cando_vm_push(vm, h);
    return 1;
}

/* binding:call(args...) */
static int native_cffi_binding_call(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        cando_vm_error(vm, "ffi: binding call requires a binding handle");
        return -1;
    }
    int bslot = bind_handle_slot(vm, args[0]);
    if (bslot < 0) {
        cando_vm_error(vm, "ffi: invalid binding handle");
        return -1;
    }
    if (g_bind_pool[bslot].lib_slot < 0 || g_bind_pool[bslot].fn == NULL) {
        cando_vm_error(vm, "ffi: binding's library was closed");
        return -1;
    }

    void   *fn  = g_bind_pool[bslot].fn;
    CffiSig sig = g_bind_pool[bslot].sig;

    return do_ffi_call(vm, fn, &sig, &args[1], argc - 1, "binding.call");
}

/* =========================================================================
 * Natives -- ffi.sizeof / ffi.alignof
 * ======================================================================= */

static int native_cffi_sizeof(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "ffi.sizeof: expected a type string");
        return -1;
    }
    const char *t = cando_as_string(args[0])->data;
    CffiKind k;
    CffiParseError perr;
    if (!cffi_parse_single_type(t, &k, &perr)) {
        cando_vm_error(vm, "ffi.sizeof: bad type \"%s\": %s", t, perr.message);
        return -1;
    }
    cando_vm_push(vm, cando_number((double)cffi_kind_size(k)));
    return 1;
}

static int native_cffi_alignof(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "ffi.alignof: expected a type string");
        return -1;
    }
    const char *t = cando_as_string(args[0])->data;
    CffiKind k;
    CffiParseError perr;
    if (!cffi_parse_single_type(t, &k, &perr)) {
        cando_vm_error(vm, "ffi.alignof: bad type \"%s\": %s", t, perr.message);
        return -1;
    }
    cando_vm_push(vm, cando_number((double)cffi_kind_align(k)));
    return 1;
}

/* =========================================================================
 * Natives -- ffi.alloc / buf methods
 * ======================================================================= */

/* ffi.alloc(nbytes) -> buffer handle */
static int native_cffi_alloc(CandoVM *vm, int argc, CandoValue *args)
{
    ensure_pools_inited();
    if (argc < 1 || !cando_is_number(args[0])) {
        cando_vm_error(vm, "ffi.alloc: expected a byte count");
        return -1;
    }
    double dn = cando_as_number(args[0]);
    if (dn < 0 || dn != (double)(int64_t)dn) {
        cando_vm_error(vm, "ffi.alloc: byte count must be a non-negative integer");
        return -1;
    }
    size_t n = (size_t)(int64_t)dn;
    void *p = calloc(n ? n : 1, 1);
    if (!p) {
        cando_vm_error(vm, "ffi.alloc: out of memory (%zu bytes)", n);
        return -1;
    }
    int slot = alloc_buf_slot();
    if (slot < 0) {
        free(p);
        cando_vm_error(vm, "ffi.alloc: buffer pool exhausted (max %d)",
                       CFFI_MAX_BUFS);
        return -1;
    }
    g_buf_pool[slot].data  = p;
    g_buf_pool[slot].len   = n;
    g_buf_pool[slot].owned = true;
    cando_vm_push(vm, make_buf_handle(vm, slot));
    return 1;
}

/* ffi.string(ptr [, len]) -> CanDo string */
static int native_cffi_string(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        cando_vm_error(vm, "ffi.string: expected a pointer");
        return -1;
    }
    if (cando_is_null(args[0])) {
        cando_vm_error(vm, "ffi.string: pointer is NULL");
        return -1;
    }
    int slot = buf_handle_slot(vm, args[0]);
    if (slot < 0) {
        cando_vm_error(vm, "ffi.string: argument is not a buffer / pointer");
        return -1;
    }
    void *data = g_buf_pool[slot].data;
    if (!data) {
        cando_vm_error(vm, "ffi.string: pointer is NULL");
        return -1;
    }

    size_t n;
    if (argc >= 2 && cando_is_number(args[1])) {
        double dn = cando_as_number(args[1]);
        if (dn < 0) {
            cando_vm_error(vm, "ffi.string: length must be non-negative");
            return -1;
        }
        n = (size_t)(int64_t)dn;
    } else {
        n = strlen((const char *)data);
    }
    libutil_push_str(vm, (const char *)data, (u32)n);
    return 1;
}

/* buf:read(offset, len) | buf:read(len) | buf:read() */
static int native_cffi_buf_read(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        cando_vm_error(vm, "ffi: buffer read requires a buffer handle");
        return -1;
    }
    int slot = buf_handle_slot(vm, args[0]);
    if (slot < 0) {
        cando_vm_error(vm, "ffi: invalid buffer handle");
        return -1;
    }
    void *data = g_buf_pool[slot].data;
    size_t blen = g_buf_pool[slot].len;
    if (!data) {
        cando_vm_error(vm, "ffi: buffer is NULL");
        return -1;
    }

    size_t off = 0, len = 0;
    if (argc == 1) {
        /* Without arguments: read the whole owned buffer.  Foreign
         * buffers don't know their length and must be told. */
        if (g_buf_pool[slot].len == 0 && !g_buf_pool[slot].owned) {
            cando_vm_error(vm,
                "ffi: cannot read foreign pointer without an explicit length");
            return -1;
        }
        len = blen;
    } else if (argc == 2) {
        if (!cando_is_number(args[1])) {
            cando_vm_error(vm, "ffi: read length must be a number");
            return -1;
        }
        len = (size_t)(int64_t)cando_as_number(args[1]);
    } else {
        if (!cando_is_number(args[1]) || !cando_is_number(args[2])) {
            cando_vm_error(vm, "ffi: read offset and length must be numbers");
            return -1;
        }
        off = (size_t)(int64_t)cando_as_number(args[1]);
        len = (size_t)(int64_t)cando_as_number(args[2]);
    }

    if (g_buf_pool[slot].owned || g_buf_pool[slot].len > 0) {
        if (off > blen || len > blen - off) {
            cando_vm_error(vm,
                "ffi: read out of bounds (offset=%zu, len=%zu, size=%zu)",
                off, len, blen);
            return -1;
        }
    }
    libutil_push_str(vm, (const char *)data + off, (u32)len);
    return 1;
}

/* buf:write(bytes) | buf:write(offset, bytes) */
static int native_cffi_buf_write(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 2) {
        cando_vm_error(vm, "ffi: buffer write requires (bytes) or (offset, bytes)");
        return -1;
    }
    int slot = buf_handle_slot(vm, args[0]);
    if (slot < 0) {
        cando_vm_error(vm, "ffi: invalid buffer handle");
        return -1;
    }
    void *data = g_buf_pool[slot].data;
    size_t blen = g_buf_pool[slot].len;
    if (!data) {
        cando_vm_error(vm, "ffi: buffer is NULL");
        return -1;
    }

    size_t off = 0;
    CandoValue bytes_val;
    if (argc == 2) {
        bytes_val = args[1];
    } else {
        if (!cando_is_number(args[1])) {
            cando_vm_error(vm, "ffi: write offset must be a number");
            return -1;
        }
        off = (size_t)(int64_t)cando_as_number(args[1]);
        bytes_val = args[2];
    }

    if (!cando_is_string(bytes_val)) {
        cando_vm_error(vm, "ffi: write bytes must be a string");
        return -1;
    }
    CandoString *s = cando_as_string(bytes_val);

    if (g_buf_pool[slot].owned || g_buf_pool[slot].len > 0) {
        if (off > blen || s->length > blen - off) {
            cando_vm_error(vm,
                "ffi: write out of bounds (offset=%zu, len=%u, size=%zu)",
                off, s->length, blen);
            return -1;
        }
    }
    memcpy((char *)data + off, s->data, s->length);
    cando_vm_push(vm, cando_null());
    return 1;
}

/* buf:address() -> integer */
static int native_cffi_buf_address(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        cando_vm_error(vm, "ffi: address requires a buffer handle");
        return -1;
    }
    int slot = buf_handle_slot(vm, args[0]);
    if (slot < 0) {
        cando_vm_error(vm, "ffi: invalid buffer handle");
        return -1;
    }
    uintptr_t a = (uintptr_t)g_buf_pool[slot].data;
    cando_vm_push(vm, cando_number((double)a));
    return 1;
}

/* buf:size() -> length, 0 if foreign + unknown */
static int native_cffi_buf_size(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        cando_vm_error(vm, "ffi: size requires a buffer handle");
        return -1;
    }
    int slot = buf_handle_slot(vm, args[0]);
    if (slot < 0) {
        cando_vm_error(vm, "ffi: invalid buffer handle");
        return -1;
    }
    cando_vm_push(vm, cando_number((double)g_buf_pool[slot].len));
    return 1;
}

/* buf:isNull() -> bool */
static int native_cffi_buf_is_null(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        cando_vm_error(vm, "ffi: isNull requires a buffer handle");
        return -1;
    }
    int slot = buf_handle_slot(vm, args[0]);
    if (slot < 0) {
        /* Freed / invalid handle => treat as "null" for ergonomic
         * checks; the script can use :free() explicitly. */
        cando_vm_push(vm, cando_bool(true));
        return 1;
    }
    cando_vm_push(vm, cando_bool(g_buf_pool[slot].data == NULL));
    return 1;
}

/* buf:free() -- frees the backing allocation if owned, marks slot free.
 * Idempotent: calling on a freed buffer is a no-op. */
static int native_cffi_buf_free(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    int slot = buf_handle_slot(vm, args[0]);
    if (slot < 0) {
        cando_vm_push(vm, cando_null());
        return 1;
    }
    CFFI_MUTEX_LOCK(&g_buf_mutex);
    if (g_buf_pool[slot].owned && g_buf_pool[slot].data) {
        free(g_buf_pool[slot].data);
    }
    g_buf_pool[slot].data   = NULL;
    g_buf_pool[slot].len    = 0;
    g_buf_pool[slot].owned  = false;
    g_buf_pool[slot].in_use = false;
    CFFI_MUTEX_UNLOCK(&g_buf_mutex);
    cando_vm_push(vm, cando_null());
    return 1;
}

/* =========================================================================
 * Natives -- ffi.errno
 * ======================================================================= */

static int native_cffi_errno(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc >= 1 && cando_is_number(args[0])) {
        errno = (int)cando_as_number(args[0]);
    }
    cando_vm_push(vm, cando_number((double)errno));
    return 1;
}

/* =========================================================================
 * Module init
 * ======================================================================= */

#if defined(_WIN32) || defined(_WIN64)
__declspec(dllexport)
#elif defined(__GNUC__)
__attribute__((visibility("default")))
#endif
CandoValue cando_module_init(CandoVM *vm)
{
    ensure_pools_inited();
    g_method_count = 0;

    CandoValue tbl = cando_bridge_new_object(vm);
    CdoObject *obj = cando_bridge_resolve(vm, cando_as_handle(tbl));

    /* Module-level constructors and queries. */
    register_method(vm, obj, "load",     native_cffi_load,    METHOD_MODULE_ONLY);
    register_method(vm, obj, "current",  native_cffi_current, METHOD_MODULE_ONLY);
    register_method(vm, obj, "sizeof",   native_cffi_sizeof,  METHOD_MODULE_ONLY);
    register_method(vm, obj, "alignof",  native_cffi_alignof, METHOD_MODULE_ONLY);
    register_method(vm, obj, "alloc",    native_cffi_alloc,   METHOD_MODULE_ONLY);
    register_method(vm, obj, "string",   native_cffi_string,  METHOD_MODULE_ONLY);
    register_method(vm, obj, "errno",    native_cffi_errno,   METHOD_MODULE_ONLY);

    /* Library handle methods.  Each is also exposed as a module-level
     * function so `ffi.call(lib, ...)` and `lib:call(...)` both work. */
    register_method(vm, obj, "call",     native_cffi_call,         METHOD_ON_LIB);
    register_method(vm, obj, "bind",     native_cffi_bind,         METHOD_ON_LIB);
    register_method(vm, obj, "close",    native_cffi_close,        METHOD_ON_LIB);

    /* Binding handle methods. */
    {
        CandoValue sentinel = cando_vm_add_native(vm, native_cffi_binding_call);
        f64 s = cando_is_number(sentinel) ? cando_as_number(sentinel) : 0.0;
        if (g_method_count < MAX_METHOD_ENTRIES) {
            g_methods[g_method_count].name     = "call";
            g_methods[g_method_count].sentinel = s;
            g_methods[g_method_count].target   = METHOD_ON_BIND;
            g_method_count++;
        }
    }

    /* Buffer / pointer handle methods. */
    register_method(vm, obj, "read",     native_cffi_buf_read,     METHOD_ON_BUF);
    register_method(vm, obj, "write",    native_cffi_buf_write,    METHOD_ON_BUF);
    register_method(vm, obj, "address",  native_cffi_buf_address,  METHOD_ON_BUF);
    register_method(vm, obj, "size",     native_cffi_buf_size,     METHOD_ON_BUF);
    register_method(vm, obj, "isNull",   native_cffi_buf_is_null,  METHOD_ON_BUF);
    register_method(vm, obj, "free",     native_cffi_buf_free,     METHOD_ON_BUF);

    /* Module-version string. */
    obj_set_string(obj, "VERSION",
                   CFFI_MODULE_VERSION,
                   (u32)sizeof(CFFI_MODULE_VERSION) - 1);

    /* Constants -- sizes of platform-dependent types for scripts that
     * want to consult them without parsing a type string. */
    obj_set_number(obj, "SIZEOF_VOIDP",  (f64)sizeof(void *));
    obj_set_number(obj, "SIZEOF_LONG",   (f64)sizeof(long));
    obj_set_number(obj, "SIZEOF_SIZE_T", (f64)sizeof(size_t));

    return tbl;
}
