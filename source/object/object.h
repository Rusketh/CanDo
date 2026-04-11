/*
 * object.h -- Core CdoObject: hash-map with FIFO key ordering.
 *
 * Defines CdoObject, ObjSlot, ObjectKind, field flags, meta-key names,
 * and the raw field-access / prototype-chain / readonly / iteration API.
 *
 * array.h, function.h, and class.h each #include this header and extend
 * the CdoObject with their specialised operations.
 *
 * Every heap object begins with CandoLockHeader (from core/lock.h) so the
 * auto-locking layer can protect it.  All memory goes through
 * cando_alloc / cando_realloc / cando_free (core/common.h).
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CDO_OBJECT_H
#define CDO_OBJECT_H

#include "common.h"
#include "lock.h"
#include "string.h"
#include "value.h"

/* -----------------------------------------------------------------------
 * Field flags
 * --------------------------------------------------------------------- */
#define FIELD_NONE    ((u8)0x00)
#define FIELD_STATIC  ((u8)0x01)  /* immutable after initial assignment  */
#define FIELD_PRIVATE ((u8)0x02)  /* hidden from outside class scope     */

/* -----------------------------------------------------------------------
 * Meta-field name constants
 * Pre-interned at cdo_object_init() time.
 * --------------------------------------------------------------------- */
#define META_INDEX     "__index"
#define META_CALL      "__call"
#define META_TYPE      "__type"
#define META_TOSTRING  "__tostring"
#define META_EQUAL     "__equal"
#define META_GREATER   "__greater"
#define META_IS        "__is"
#define META_NEGATE    "__negate"
#define META_NOT       "__not"
#define META_ADD       "__add"
#define META_LEN       "__len"
#define META_NEWINDEX  "__newindex"

/* -----------------------------------------------------------------------
 * Native function signature
 * --------------------------------------------------------------------- */
typedef struct CdoState CdoState;  /* VM state -- defined elsewhere */
typedef CdoValue (*CdoNativeFn)(CdoState *state, CdoValue *args, u32 argc);

/* -----------------------------------------------------------------------
 * ObjSlot -- one entry in the object's open-addressing hash table
 *
 * Keys are interned CdoStrings; pointer equality == content equality.
 * NULL key  = slot empty.
 * Tombstone = slot deleted (indicated by cdo_objslot_tombstone sentinel).
 * --------------------------------------------------------------------- */
typedef struct {
    CdoString *key;       /* NULL = empty, tombstone sentinel, or interned */
    CdoValue   value;
    u8         flags;     /* FIELD_* bitmask                               */
    u32        fifo_prev; /* insertion-order list -- UINT32_MAX = none     */
    u32        fifo_next;
} ObjSlot;

/* -----------------------------------------------------------------------
 * ObjectKind -- specialisation tag stored inside CdoObject
 * --------------------------------------------------------------------- */
typedef enum {
    OBJ_OBJECT   = 0,  /* plain key-value object                          */
    OBJ_ARRAY    = 1,  /* numeric-indexed array (dense storage)           */
    OBJ_FUNCTION = 2,  /* script function / closure                       */
    OBJ_NATIVE   = 3,  /* native C function wrapper                       */
    OBJ_THREAD   = 4,  /* spawned OS thread (CdoThread*)                  */
} ObjectKind;

/* -----------------------------------------------------------------------
 * CdoObject
 *
 * CandoLockHeader MUST be the first member (offset 0) so a CdoObject*
 * can be cast to CandoLockHeader* for the auto-locking layer.
 * --------------------------------------------------------------------- */
struct CdoObject {
    CandoLockHeader lock;          /* thread-safety header -- offset 0    */
    u8              kind;          /* ObjectKind                          */
    bool            readonly;      /* no new fields and no writes         */

    /* Hash table: open addressing, linear probing, cap always power of 2 */
    ObjSlot        *slots;
    u32             slot_cap;      /* allocated slot count                */
    u32             field_count;   /* live fields (excluding tombstones)  */
    u32             tombstone_count;

    /* FIFO ordering: doubly-linked list through slots[] indices          */
    u32             fifo_head;     /* index of first inserted slot        */
    u32             fifo_tail;     /* index of last inserted slot         */

    /* Array-specific dense storage (valid when kind == OBJ_ARRAY)       */
    CdoValue       *items;         /* 0-based dense item array            */
    u32             items_len;     /* number of items in use              */
    u32             items_cap;     /* allocated capacity                  */

    /* User-level mutex for object.lock / object.unlock.
     * Separate from the internal RW `lock` above so that script-held locks
     * never interact with internal VM field reads/writes.                */
    _Atomic(u64)    user_lock_id;    /* owning thread ID, 0 = free       */
    _Atomic(u32)    user_lock_depth; /* re-entrancy depth counter        */

    /* Function-specific data                                             */
    union {
        struct {                   /* OBJ_FUNCTION: script closure        */
            u32       param_count;
            u32       upvalue_count;
            CdoValue *upvalues;    /* captured upvalue array              */
            void     *bytecode;    /* opaque JIT bytecode pointer         */
        } script;
        struct {                   /* OBJ_NATIVE: C function wrapper      */
            CdoNativeFn fn;
            u32         param_count;
        } native;
    } fn;
};

/* -----------------------------------------------------------------------
 * Internal allocation helper (used by array.c, function.c, class.c)
 *
 * Allocates and zero-initialises a CdoObject of the given kind.
 * Not for direct use by VM-layer code; use cdo_object_new() etc.
 * --------------------------------------------------------------------- */
CdoObject *cdo_obj_alloc(ObjectKind kind);

/* -----------------------------------------------------------------------
 * Global initialisation / teardown
 *
 * Call cdo_object_init() once before using any other function in this
 * module.  cdo_object_destroy_globals() tears down the intern table and
 * cached meta-key pointers.
 * --------------------------------------------------------------------- */
CANDO_API void cdo_object_init(void);
CANDO_API void cdo_object_destroy_globals(void);

/* -----------------------------------------------------------------------
 * Object creation
 * --------------------------------------------------------------------- */
CANDO_API CdoObject *cdo_object_new(void);
CANDO_API void       cdo_object_destroy(CdoObject *obj);

/* -----------------------------------------------------------------------
 * Raw field access (no meta-method dispatch, no prototype chain)
 *
 * All key strings SHOULD be interned (via cdo_string_intern) for correct
 * pointer-equality lookup.
 * --------------------------------------------------------------------- */

/*
 * cdo_object_rawget -- look up key in obj's own fields.
 * Returns true and writes *out if found.  Acquires read lock.
 */
CANDO_API bool cdo_object_rawget(const CdoObject *obj, CdoString *key, CdoValue *out);

/*
 * cdo_object_rawset -- insert or update a field.
 * Returns false if: obj is readonly, key is FIELD_STATIC and already set,
 * or key is NULL.  Acquires write lock.
 */
CANDO_API bool cdo_object_rawset(CdoObject *obj, CdoString *key, CdoValue val, u8 flags);

/*
 * cdo_object_rawdelete -- remove a field by key.
 * Returns false if not found, obj is readonly, or field is FIELD_STATIC.
 * Acquires write lock.
 */
CANDO_API bool cdo_object_rawdelete(CdoObject *obj, CdoString *key);

/* -----------------------------------------------------------------------
 * Prototype-chain lookup (__index traversal)
 *
 * Looks up key in obj; if not found AND obj has an __index field that is
 * an object, recurses up to CANDO_PROTO_DEPTH_MAX levels.
 * --------------------------------------------------------------------- */
#define CANDO_PROTO_DEPTH_MAX 32

CANDO_API bool cdo_object_get(CdoObject *obj, CdoString *key, CdoValue *out);

/* -----------------------------------------------------------------------
 * Readonly flag
 * --------------------------------------------------------------------- */
CANDO_API void cdo_object_set_readonly(CdoObject *obj, bool ro);
CANDO_API bool cdo_object_is_readonly(const CdoObject *obj);

/* -----------------------------------------------------------------------
 * FIFO iteration (insertion order)
 *
 * Calls fn for each live field in insertion order.
 * Stops early if fn returns false.
 * --------------------------------------------------------------------- */
typedef bool (*CdoIterFn)(CdoString *key, CdoValue *val, u8 flags, void *ud);
CANDO_API void cdo_object_foreach(const CdoObject *obj, CdoIterFn fn, void *ud);

/* -----------------------------------------------------------------------
 * Object length
 *
 * For arrays: items_len.
 * For plain objects: field_count.
 * (__len meta-method dispatch is handled by the VM layer.)
 * --------------------------------------------------------------------- */
CANDO_API u32 cdo_object_length(const CdoObject *obj);

/* -----------------------------------------------------------------------
 * Cached interned meta-key pointers (valid after cdo_object_init())
 *
 * Pre-interned for efficient meta-method lookups.
 * Do NOT release them -- owned by the intern table.
 * --------------------------------------------------------------------- */
extern CdoString *g_meta_index;
extern CdoString *g_meta_call;
extern CdoString *g_meta_type;
extern CdoString *g_meta_tostring;
extern CdoString *g_meta_equal;
extern CdoString *g_meta_greater;
extern CdoString *g_meta_is;
extern CdoString *g_meta_negate;
extern CdoString *g_meta_not;
extern CdoString *g_meta_add;
extern CdoString *g_meta_len;
extern CdoString *g_meta_newindex;

#endif /* CDO_OBJECT_H */
