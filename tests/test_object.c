/*
 * tests/test_object.c -- Unit tests for source/object: CdoObject, CdoString,
 *                        string interning, arrays, functions, prototype chains.
 *
 * Compile:
 *   gcc -std=c11 -pthread -D_GNU_SOURCE \
 *       -iquote source/core -iquote source/object \
 *       source/core/common.c source/core/lock.c \
 *       source/object/string.c source/object/value.c source/object/object.c \
 *       source/object/array.c source/object/function.c source/object/class.c \
 *       tests/test_object.c -o tests/test_object
 *   ./tests/test_object
 *
 * Exit 0 on success, non-zero on failure.
 */

#include "common.h"
#include "object.h"
#include "array.h"
#include "function.h"
#include "class.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>

/* -----------------------------------------------------------------------
 * Minimal test harness (mirrors test_core.c)
 * --------------------------------------------------------------------- */
static int g_run    = 0;
static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) static void name(void)

#define EXPECT(cond) \
    do { \
        g_run++; \
        if (cond) { g_passed++; } \
        else { g_failed++; \
               fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define EXPECT_TRUE(x)   EXPECT(!!(x))
#define EXPECT_FALSE(x)  EXPECT(!(x))
#define EXPECT_EQ(a, b)  EXPECT((a) == (b))
#define EXPECT_NEQ(a, b) EXPECT((a) != (b))
#define EXPECT_STR(a, b) EXPECT(strcmp((a), (b)) == 0)

static void run_test(const char *name, void (*fn)(void)) {
    printf("  %-48s ", name);
    fflush(stdout);
    int before = g_failed;
    fn();
    printf("%s\n", g_failed == before ? "OK" : "FAILED");
}

/* -----------------------------------------------------------------------
 * CdoString tests
 * --------------------------------------------------------------------- */
TEST(test_string_new_release) {
    CdoString *s = cdo_string_new("hello", 5);
    EXPECT_EQ(s->ref_count, 1u);
    EXPECT_EQ(s->length, 5u);
    EXPECT_STR(s->data, "hello");
    EXPECT_FALSE(s->interned);
    cdo_string_release(s);  /* frees */
}

TEST(test_string_retain) {
    CdoString *s = cdo_string_new("world", 5);
    cdo_string_retain(s);
    EXPECT_EQ(s->ref_count, 2u);
    cdo_string_release(s);
    EXPECT_EQ(s->ref_count, 1u);
    cdo_string_release(s);
}

TEST(test_string_hash) {
    CdoString *s = cdo_string_new("abc", 3);
    u32 h1 = cdo_string_hash(s);
    u32 h2 = cdo_string_hash(s);
    EXPECT_EQ(h1, h2);        /* stable */
    EXPECT_NEQ(h1, 0u);
    cdo_string_release(s);
}

/* -----------------------------------------------------------------------
 * String interning tests
 * --------------------------------------------------------------------- */
TEST(test_intern_same_pointer) {
    CdoString *a = cdo_string_intern("foo", 3);
    CdoString *b = cdo_string_intern("foo", 3);
    EXPECT_EQ(a, b);  /* must be the same pointer */
    EXPECT_TRUE(a->interned);
    cdo_string_release(a);
    cdo_string_release(b);
}

TEST(test_intern_different_content) {
    CdoString *a = cdo_string_intern("foo", 3);
    CdoString *b = cdo_string_intern("bar", 3);
    EXPECT_NEQ(a, b);
    cdo_string_release(a);
    cdo_string_release(b);
}

TEST(test_intern_ref_count) {
    CdoString *a = cdo_string_intern("baz", 3);
    /* intern table holds 1 ref, caller got +1 */
    EXPECT_EQ(a->ref_count, 2u);
    cdo_string_release(a);  /* release caller ref; table ref remains */
}

/* -----------------------------------------------------------------------
 * CdoValue tests
 * --------------------------------------------------------------------- */
TEST(test_value_null) {
    CdoValue v = cdo_null();
    EXPECT_TRUE(cdo_is_null(v));
    EXPECT_FALSE(cdo_is_bool(v));
    char *s = cdo_value_tostring(v);
    EXPECT_STR(s, "null");
    free(s);
}

TEST(test_value_bool) {
    CdoValue t = cdo_bool(true);
    CdoValue f = cdo_bool(false);
    EXPECT_TRUE(cdo_is_bool(t));
    EXPECT_TRUE(t.as.boolean);
    EXPECT_FALSE(f.as.boolean);
}

TEST(test_value_number) {
    CdoValue v = cdo_number(3.14);
    EXPECT_TRUE(cdo_is_number(v));
    EXPECT_EQ(v.as.number, 3.14);
}

TEST(test_value_string) {
    CdoString *s = cdo_string_new("hi", 2);
    CdoValue   v = cdo_string_value(s);
    EXPECT_TRUE(cdo_is_string(v));
    char *r = cdo_value_tostring(v);
    EXPECT_STR(r, "hi");
    free(r);
    cdo_string_release(s);
}

TEST(test_value_equal) {
    EXPECT_TRUE(cdo_value_equal(cdo_null(),      cdo_null()));
    EXPECT_TRUE(cdo_value_equal(cdo_bool(true),  cdo_bool(true)));
    EXPECT_FALSE(cdo_value_equal(cdo_bool(true), cdo_bool(false)));
    EXPECT_TRUE(cdo_value_equal(cdo_number(1.0), cdo_number(1.0)));
    EXPECT_FALSE(cdo_value_equal(cdo_number(1.0), cdo_number(2.0)));
    EXPECT_FALSE(cdo_value_equal(cdo_null(),     cdo_bool(false)));

    CdoString *s1 = cdo_string_new("abc", 3);
    CdoString *s2 = cdo_string_new("abc", 3);
    EXPECT_TRUE(cdo_value_equal(cdo_string_value(s1), cdo_string_value(s2)));
    cdo_string_release(s1);
    cdo_string_release(s2);
}

TEST(test_value_copy_release) {
    CdoString *s = cdo_string_new("dup", 3);
    CdoValue   v = cdo_string_value(s);
    CdoValue   c = cdo_value_copy(v);
    EXPECT_EQ(s->ref_count, 2u);
    cdo_value_release(c);
    EXPECT_EQ(s->ref_count, 1u);
    cdo_string_release(s);
}

/* -----------------------------------------------------------------------
 * Basic object tests
 * --------------------------------------------------------------------- */
TEST(test_object_new_destroy) {
    CdoObject *obj = cdo_object_new();
    EXPECT_TRUE(obj != NULL);
    EXPECT_EQ(obj->kind, (u8)OBJ_OBJECT);
    EXPECT_EQ(obj->field_count, 0u);
    EXPECT_FALSE(obj->readonly);
    cdo_object_destroy(obj);
}

TEST(test_object_rawset_rawget) {
    CdoObject *obj = cdo_object_new();
    CdoString *k   = cdo_string_intern("x", 1);

    EXPECT_TRUE(cdo_object_rawset(obj, k, cdo_number(42.0), FIELD_NONE));

    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(obj, k, &out));
    EXPECT_TRUE(cdo_is_number(out));
    EXPECT_EQ(out.as.number, 42.0);

    EXPECT_EQ(obj->field_count, 1u);

    cdo_string_release(k);
    cdo_object_destroy(obj);
}

TEST(test_object_rawget_missing) {
    CdoObject *obj = cdo_object_new();
    CdoString *k   = cdo_string_intern("missing", 7);

    CdoValue out;
    EXPECT_FALSE(cdo_object_rawget(obj, k, &out));

    cdo_string_release(k);
    cdo_object_destroy(obj);
}

TEST(test_object_rawset_update) {
    CdoObject *obj = cdo_object_new();
    CdoString *k   = cdo_string_intern("v", 1);

    cdo_object_rawset(obj, k, cdo_number(1.0), FIELD_NONE);
    cdo_object_rawset(obj, k, cdo_number(2.0), FIELD_NONE);
    EXPECT_EQ(obj->field_count, 1u);

    CdoValue out;
    cdo_object_rawget(obj, k, &out);
    EXPECT_EQ(out.as.number, 2.0);

    cdo_string_release(k);
    cdo_object_destroy(obj);
}

TEST(test_object_rawdelete) {
    CdoObject *obj = cdo_object_new();
    CdoString *k   = cdo_string_intern("del", 3);

    cdo_object_rawset(obj, k, cdo_number(5.0), FIELD_NONE);
    EXPECT_EQ(obj->field_count, 1u);
    EXPECT_TRUE(cdo_object_rawdelete(obj, k));
    EXPECT_EQ(obj->field_count, 0u);

    CdoValue out;
    EXPECT_FALSE(cdo_object_rawget(obj, k, &out));

    cdo_string_release(k);
    cdo_object_destroy(obj);
}

TEST(test_object_readonly) {
    CdoObject *obj = cdo_object_new();
    CdoString *k   = cdo_string_intern("r", 1);

    cdo_object_rawset(obj, k, cdo_number(1.0), FIELD_NONE);
    cdo_object_set_readonly(obj, true);
    EXPECT_TRUE(cdo_object_is_readonly(obj));

    /* Writes should fail. */
    EXPECT_FALSE(cdo_object_rawset(obj, k, cdo_number(2.0), FIELD_NONE));
    EXPECT_FALSE(cdo_object_rawdelete(obj, k));

    /* Read still works. */
    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(obj, k, &out));
    EXPECT_EQ(out.as.number, 1.0);

    cdo_string_release(k);
    cdo_object_destroy(obj);
}

/* -----------------------------------------------------------------------
 * Field flag tests
 * --------------------------------------------------------------------- */
TEST(test_field_static) {
    CdoObject *obj = cdo_object_new();
    CdoString *k   = cdo_string_intern("version", 7);

    /* Insert with FIELD_STATIC. */
    EXPECT_TRUE(cdo_object_rawset(obj, k, cdo_number(1.0), FIELD_STATIC));
    /* Updating a static field must fail. */
    EXPECT_FALSE(cdo_object_rawset(obj, k, cdo_number(2.0), FIELD_NONE));
    /* Value is unchanged. */
    CdoValue out;
    cdo_object_rawget(obj, k, &out);
    EXPECT_EQ(out.as.number, 1.0);
    /* Deleting a static field must fail. */
    EXPECT_FALSE(cdo_object_rawdelete(obj, k));

    cdo_string_release(k);
    cdo_object_destroy(obj);
}

TEST(test_field_private_flag_stored) {
    CdoObject *obj = cdo_object_new();
    CdoString *k   = cdo_string_intern("secret", 6);

    cdo_object_rawset(obj, k, cdo_number(99.0), FIELD_PRIVATE);

    /* Verify the flag is preserved in the slot. */
    u32 idx = obj->fifo_head;
    EXPECT_NEQ(idx, UINT32_MAX);
    EXPECT_TRUE(!!(obj->slots[idx].flags & FIELD_PRIVATE));

    cdo_string_release(k);
    cdo_object_destroy(obj);
}

/* -----------------------------------------------------------------------
 * FIFO ordering tests
 * --------------------------------------------------------------------- */
typedef struct { const char **keys; int count; } FifoCtx;

static bool collect_keys(CdoString *key, CdoValue *val, u8 flags, void *ud) {
    CANDO_UNUSED(val); CANDO_UNUSED(flags);
    FifoCtx *ctx = ud;
    if (ctx->count < 16)
        ctx->keys[ctx->count++] = key->data;
    return true;
}

TEST(test_fifo_insertion_order) {
    CdoObject *obj = cdo_object_new();
    const char *names[] = { "c", "a", "b", "z", "m" };

    for (int i = 0; i < 5; i++) {
        CdoString *k = cdo_string_intern(names[i], (u32)strlen(names[i]));
        cdo_object_rawset(obj, k, cdo_number((f64)i), FIELD_NONE);
        cdo_string_release(k);
    }

    const char *order[16];
    FifoCtx ctx = { order, 0 };
    cdo_object_foreach(obj, collect_keys, &ctx);

    EXPECT_EQ(ctx.count, 5);
    for (int i = 0; i < 5; i++)
        EXPECT_STR(order[i], names[i]);

    cdo_object_destroy(obj);
}

TEST(test_fifo_after_delete) {
    CdoObject *obj = cdo_object_new();
    const char *names[] = { "x", "y", "z" };
    for (int i = 0; i < 3; i++) {
        CdoString *k = cdo_string_intern(names[i], 1);
        cdo_object_rawset(obj, k, cdo_number((f64)i), FIELD_NONE);
        cdo_string_release(k);
    }

    /* Delete "y" (middle element). */
    CdoString *ky = cdo_string_intern("y", 1);
    cdo_object_rawdelete(obj, ky);
    cdo_string_release(ky);

    const char *order[16];
    FifoCtx ctx = { order, 0 };
    cdo_object_foreach(obj, collect_keys, &ctx);

    EXPECT_EQ(ctx.count, 2);
    EXPECT_STR(order[0], "x");
    EXPECT_STR(order[1], "z");

    cdo_object_destroy(obj);
}

/* -----------------------------------------------------------------------
 * Hash table growth / rehash
 * --------------------------------------------------------------------- */
TEST(test_many_fields) {
    CdoObject *obj = cdo_object_new();
    char buf[32];

    /* Insert 64 fields to force multiple rehashes. */
    for (int i = 0; i < 64; i++) {
        snprintf(buf, sizeof(buf), "field_%d", i);
        CdoString *k = cdo_string_intern(buf, (u32)strlen(buf));
        cdo_object_rawset(obj, k, cdo_number((f64)i), FIELD_NONE);
        cdo_string_release(k);
    }
    EXPECT_EQ(obj->field_count, 64u);

    /* Verify all fields are accessible. */
    bool all_ok = true;
    for (int i = 0; i < 64; i++) {
        snprintf(buf, sizeof(buf), "field_%d", i);
        CdoString *k = cdo_string_intern(buf, (u32)strlen(buf));
        CdoValue out;
        if (!cdo_object_rawget(obj, k, &out) || out.as.number != (f64)i)
            all_ok = false;
        cdo_string_release(k);
    }
    EXPECT_TRUE(all_ok);

    cdo_object_destroy(obj);
}

/* -----------------------------------------------------------------------
 * Array tests
 * --------------------------------------------------------------------- */
TEST(test_array_new) {
    CdoObject *arr = cdo_array_new();
    EXPECT_EQ(arr->kind, (u8)OBJ_ARRAY);
    EXPECT_EQ(cdo_array_len(arr), 0u);
    cdo_object_destroy(arr);
}

TEST(test_array_push_get) {
    CdoObject *arr = cdo_array_new();
    cdo_array_push(arr, cdo_number(10.0));
    cdo_array_push(arr, cdo_number(20.0));
    cdo_array_push(arr, cdo_number(30.0));

    EXPECT_EQ(cdo_array_len(arr), 3u);

    CdoValue v;
    cdo_array_rawget_idx(arr, 0, &v); EXPECT_EQ(v.as.number, 10.0);
    cdo_array_rawget_idx(arr, 1, &v); EXPECT_EQ(v.as.number, 20.0);
    cdo_array_rawget_idx(arr, 2, &v); EXPECT_EQ(v.as.number, 30.0);

    EXPECT_FALSE(cdo_array_rawget_idx(arr, 3, &v));

    cdo_object_destroy(arr);
}

TEST(test_array_set_idx) {
    CdoObject *arr = cdo_array_new();
    cdo_array_rawset_idx(arr, 2, cdo_number(99.0));
    EXPECT_EQ(cdo_array_len(arr), 3u);

    CdoValue v;
    cdo_array_rawget_idx(arr, 0, &v); EXPECT_TRUE(cdo_is_null(v));
    cdo_array_rawget_idx(arr, 1, &v); EXPECT_TRUE(cdo_is_null(v));
    cdo_array_rawget_idx(arr, 2, &v); EXPECT_EQ(v.as.number, 99.0);

    cdo_object_destroy(arr);
}

TEST(test_array_readonly) {
    CdoObject *arr = cdo_array_new();
    cdo_array_push(arr, cdo_number(1.0));
    cdo_object_set_readonly(arr, true);
    EXPECT_FALSE(cdo_array_push(arr, cdo_number(2.0)));
    EXPECT_EQ(cdo_array_len(arr), 1u);
    cdo_object_destroy(arr);
}

/* -----------------------------------------------------------------------
 * Function object tests
 * --------------------------------------------------------------------- */
TEST(test_function_new) {
    CdoObject *fn = cdo_function_new(2, (void *)0xDEAD, NULL, 0);
    EXPECT_EQ(fn->kind, (u8)OBJ_FUNCTION);
    EXPECT_EQ(fn->fn.script.param_count, 2u);
    EXPECT_EQ(fn->fn.script.bytecode, (void *)0xDEAD);
    EXPECT_EQ(fn->fn.script.upvalue_count, 0u);
    cdo_object_destroy(fn);
}

TEST(test_function_upvalues) {
    CdoValue upvals[2] = { cdo_number(1.0), cdo_number(2.0) };
    CdoObject *fn = cdo_function_new(0, NULL, upvals, 2);
    EXPECT_EQ(fn->fn.script.upvalue_count, 2u);
    EXPECT_EQ(fn->fn.script.upvalues[0].as.number, 1.0);
    EXPECT_EQ(fn->fn.script.upvalues[1].as.number, 2.0);
    cdo_object_destroy(fn);
}

static CdoValue dummy_native(CdoState *s, CdoValue *args, u32 argc) {
    CANDO_UNUSED(s); CANDO_UNUSED(args); CANDO_UNUSED(argc);
    return cdo_number(42.0);
}

TEST(test_native_new) {
    CdoObject *n = cdo_native_new(dummy_native, 1);
    EXPECT_EQ(n->kind, (u8)OBJ_NATIVE);
    EXPECT_EQ(n->fn.native.fn, dummy_native);
    EXPECT_EQ(n->fn.native.param_count, 1u);
    cdo_object_destroy(n);
}

/* -----------------------------------------------------------------------
 * Class construction test
 * --------------------------------------------------------------------- */
TEST(test_class_new) {
    CdoString *type_name = cdo_string_intern("MyClass", 7);
    CdoObject *proto     = cdo_object_new();
    CdoObject *cls       = cdo_class_new(type_name, proto);

    /* __type must be set as static. */
    CdoValue type_val;
    EXPECT_TRUE(cdo_object_rawget(cls, g_meta_type, &type_val));
    EXPECT_TRUE(cdo_is_string(type_val));
    EXPECT_STR(type_val.as.string->data, "MyClass");

    /* __type must be immutable (FIELD_STATIC). */
    EXPECT_FALSE(cdo_object_rawset(cls, g_meta_type,
                                   cdo_string_value(type_name), FIELD_NONE));

    /* __index must point to proto. */
    CdoValue idx_val;
    EXPECT_TRUE(cdo_object_rawget(cls, g_meta_index, &idx_val));
    EXPECT_TRUE(cdo_is_any_object(idx_val));
    EXPECT_EQ(idx_val.as.object, proto);

    cdo_string_release(type_name);
    cdo_object_destroy(cls);
    cdo_object_destroy(proto);
}

/* -----------------------------------------------------------------------
 * Prototype chain traversal tests
 * --------------------------------------------------------------------- */
TEST(test_prototype_chain_basic) {
    /* parent.method = 99 */
    CdoObject *parent = cdo_object_new();
    CdoString *km     = cdo_string_intern("method", 6);
    cdo_object_rawset(parent, km, cdo_number(99.0), FIELD_NONE);

    /* child.__index = parent */
    CdoObject *child = cdo_object_new();
    CdoValue   pv;
    pv.tag       = CDO_OBJECT;
    pv.as.object = parent;
    cdo_object_rawset(child, g_meta_index, pv, FIELD_NONE);

    /* child should find method via __index traversal */
    CdoValue out;
    EXPECT_TRUE(cdo_object_get(child, km, &out));
    EXPECT_EQ(out.as.number, 99.0);

    /* Own field shadows prototype. */
    cdo_object_rawset(child, km, cdo_number(1.0), FIELD_NONE);
    EXPECT_TRUE(cdo_object_get(child, km, &out));
    EXPECT_EQ(out.as.number, 1.0);

    cdo_string_release(km);
    cdo_object_destroy(child);
    cdo_object_destroy(parent);
}

TEST(test_prototype_chain_depth) {
    /* Build a 3-level chain: a -> b -> c */
    CdoObject *a = cdo_object_new();
    CdoObject *b = cdo_object_new();
    CdoObject *c = cdo_object_new();
    CdoString *k = cdo_string_intern("deep", 4);

    cdo_object_rawset(c, k, cdo_number(7.0), FIELD_NONE);

    CdoValue bv = { .tag = CDO_OBJECT, .as = { .object = c } };
    CdoValue av = { .tag = CDO_OBJECT, .as = { .object = b } };
    cdo_object_rawset(b, g_meta_index, bv, FIELD_NONE);
    cdo_object_rawset(a, g_meta_index, av, FIELD_NONE);

    CdoValue out;
    EXPECT_TRUE(cdo_object_get(a, k, &out));
    EXPECT_EQ(out.as.number, 7.0);

    cdo_string_release(k);
    cdo_object_destroy(a);
    cdo_object_destroy(b);
    cdo_object_destroy(c);
}

TEST(test_prototype_self_loop_guard) {
    CdoObject *obj = cdo_object_new();
    CdoString *k   = cdo_string_intern("missing", 7);

    /* Point __index at self -- must not infinite-loop. */
    CdoValue sv = { .tag = CDO_OBJECT, .as = { .object = obj } };
    cdo_object_rawset(obj, g_meta_index, sv, FIELD_NONE);

    CdoValue out;
    EXPECT_FALSE(cdo_object_get(obj, k, &out));

    cdo_string_release(k);
    cdo_object_destroy(obj);
}

/* -----------------------------------------------------------------------
 * Object length
 * --------------------------------------------------------------------- */
TEST(test_object_length) {
    CdoObject *obj = cdo_object_new();
    CdoString *k1  = cdo_string_intern("a", 1);
    CdoString *k2  = cdo_string_intern("b", 1);

    EXPECT_EQ(cdo_object_length(obj), 0u);
    cdo_object_rawset(obj, k1, cdo_number(1.0), FIELD_NONE);
    EXPECT_EQ(cdo_object_length(obj), 1u);
    cdo_object_rawset(obj, k2, cdo_number(2.0), FIELD_NONE);
    EXPECT_EQ(cdo_object_length(obj), 2u);

    cdo_string_release(k1);
    cdo_string_release(k2);
    cdo_object_destroy(obj);
}

TEST(test_array_length) {
    CdoObject *arr = cdo_array_new();
    EXPECT_EQ(cdo_object_length(arr), 0u);
    cdo_array_push(arr, cdo_number(1.0));
    cdo_array_push(arr, cdo_number(2.0));
    EXPECT_EQ(cdo_object_length(arr), 2u);
    cdo_object_destroy(arr);
}

/* -----------------------------------------------------------------------
 * Array insert / remove tests
 * --------------------------------------------------------------------- */
TEST(test_array_insert_middle) {
    CdoObject *arr = cdo_array_new();
    cdo_array_push(arr, cdo_number(1.0));
    cdo_array_push(arr, cdo_number(3.0));

    /* Insert 2 at index 1 → [1, 2, 3] */
    EXPECT_TRUE(cdo_array_insert(arr, 1, cdo_number(2.0)));
    EXPECT_EQ(cdo_array_len(arr), 3u);

    CdoValue v;
    cdo_array_rawget_idx(arr, 0, &v); EXPECT_EQ(v.as.number, 1.0);
    cdo_array_rawget_idx(arr, 1, &v); EXPECT_EQ(v.as.number, 2.0);
    cdo_array_rawget_idx(arr, 2, &v); EXPECT_EQ(v.as.number, 3.0);

    cdo_object_destroy(arr);
}

TEST(test_array_insert_end) {
    CdoObject *arr = cdo_array_new();
    cdo_array_push(arr, cdo_number(10.0));

    /* Insert at len == append. */
    EXPECT_TRUE(cdo_array_insert(arr, 1, cdo_number(20.0)));
    EXPECT_EQ(cdo_array_len(arr), 2u);

    CdoValue v;
    cdo_array_rawget_idx(arr, 1, &v);
    EXPECT_EQ(v.as.number, 20.0);

    cdo_object_destroy(arr);
}

TEST(test_array_insert_beyond_len) {
    CdoObject *arr = cdo_array_new();
    cdo_array_push(arr, cdo_number(5.0));

    /* Index beyond len is clamped to len (append). */
    EXPECT_TRUE(cdo_array_insert(arr, 100, cdo_number(6.0)));
    EXPECT_EQ(cdo_array_len(arr), 2u);

    CdoValue v;
    cdo_array_rawget_idx(arr, 1, &v);
    EXPECT_EQ(v.as.number, 6.0);

    cdo_object_destroy(arr);
}

TEST(test_array_remove_middle) {
    CdoObject *arr = cdo_array_new();
    cdo_array_push(arr, cdo_number(10.0));
    cdo_array_push(arr, cdo_number(20.0));
    cdo_array_push(arr, cdo_number(30.0));

    CdoValue removed;
    EXPECT_TRUE(cdo_array_remove(arr, 1, &removed));
    EXPECT_EQ(removed.as.number, 20.0);
    cdo_value_release(removed);

    EXPECT_EQ(cdo_array_len(arr), 2u);

    CdoValue v;
    cdo_array_rawget_idx(arr, 0, &v); EXPECT_EQ(v.as.number, 10.0);
    cdo_array_rawget_idx(arr, 1, &v); EXPECT_EQ(v.as.number, 30.0);

    cdo_object_destroy(arr);
}

TEST(test_array_remove_first) {
    CdoObject *arr = cdo_array_new();
    cdo_array_push(arr, cdo_number(1.0));
    cdo_array_push(arr, cdo_number(2.0));

    CdoValue removed;
    EXPECT_TRUE(cdo_array_remove(arr, 0, &removed));
    EXPECT_EQ(removed.as.number, 1.0);
    cdo_value_release(removed);

    EXPECT_EQ(cdo_array_len(arr), 1u);

    CdoValue v;
    cdo_array_rawget_idx(arr, 0, &v);
    EXPECT_EQ(v.as.number, 2.0);

    cdo_object_destroy(arr);
}

TEST(test_array_remove_oob) {
    CdoObject *arr = cdo_array_new();
    cdo_array_push(arr, cdo_number(1.0));

    /* Index 5 is out of bounds. */
    CdoValue removed;
    EXPECT_FALSE(cdo_array_remove(arr, 5, &removed));
    EXPECT_EQ(cdo_array_len(arr), 1u);

    cdo_object_destroy(arr);
}

TEST(test_array_remove_readonly) {
    CdoObject *arr = cdo_array_new();
    cdo_array_push(arr, cdo_number(1.0));
    cdo_object_set_readonly(arr, true);

    CdoValue removed;
    EXPECT_FALSE(cdo_array_remove(arr, 0, &removed));
    EXPECT_EQ(cdo_array_len(arr), 1u);

    cdo_object_destroy(arr);
}

/* -----------------------------------------------------------------------
 * Meta-key constants tests
 * --------------------------------------------------------------------- */
TEST(test_meta_keys_interned) {
    /* All g_meta_* pointers must be non-NULL after cdo_object_init(). */
    EXPECT_TRUE(g_meta_index    != NULL);
    EXPECT_TRUE(g_meta_call     != NULL);
    EXPECT_TRUE(g_meta_type     != NULL);
    EXPECT_TRUE(g_meta_tostring != NULL);
    EXPECT_TRUE(g_meta_eq       != NULL);
    EXPECT_TRUE(g_meta_lt       != NULL);
    EXPECT_TRUE(g_meta_le       != NULL);
    EXPECT_TRUE(g_meta_add      != NULL);
    EXPECT_TRUE(g_meta_sub      != NULL);
    EXPECT_TRUE(g_meta_mul      != NULL);
    EXPECT_TRUE(g_meta_div      != NULL);
    EXPECT_TRUE(g_meta_mod      != NULL);
    EXPECT_TRUE(g_meta_pow      != NULL);
    EXPECT_TRUE(g_meta_unm      != NULL);
    EXPECT_TRUE(g_meta_idiv     != NULL);
    EXPECT_TRUE(g_meta_len      != NULL);
    EXPECT_TRUE(g_meta_newindex != NULL);
}

TEST(test_meta_keys_content) {
    EXPECT_STR(g_meta_index->data,    "__index");
    EXPECT_STR(g_meta_call->data,     "__call");
    EXPECT_STR(g_meta_type->data,     "__type");
    EXPECT_STR(g_meta_tostring->data, "__tostring");
    EXPECT_STR(g_meta_eq->data,       "__eq");
    EXPECT_STR(g_meta_lt->data,       "__lt");
    EXPECT_STR(g_meta_le->data,       "__le");
    EXPECT_STR(g_meta_add->data,      "__add");
    EXPECT_STR(g_meta_sub->data,      "__sub");
    EXPECT_STR(g_meta_mul->data,      "__mul");
    EXPECT_STR(g_meta_div->data,      "__div");
    EXPECT_STR(g_meta_mod->data,      "__mod");
    EXPECT_STR(g_meta_pow->data,      "__pow");
    EXPECT_STR(g_meta_unm->data,      "__unm");
    EXPECT_STR(g_meta_idiv->data,     "__idiv");
    EXPECT_STR(g_meta_len->data,      "__len");
    EXPECT_STR(g_meta_newindex->data, "__newindex");
}

TEST(test_meta_keys_unique_pointers) {
    /* Every meta-key must be a distinct interned pointer. */
    CdoString *keys[] = {
        g_meta_index, g_meta_call, g_meta_type, g_meta_tostring,
        g_meta_eq, g_meta_lt, g_meta_le,
        g_meta_add, g_meta_sub, g_meta_mul, g_meta_div,
        g_meta_mod, g_meta_pow, g_meta_unm, g_meta_idiv,
        g_meta_len, g_meta_newindex
    };
    int n = (int)(sizeof(keys) / sizeof(keys[0]));
    bool all_unique = true;
    for (int i = 0; i < n && all_unique; i++)
        for (int j = i + 1; j < n && all_unique; j++)
            if (keys[i] == keys[j]) all_unique = false;
    EXPECT_TRUE(all_unique);
}

TEST(test_meta_keys_intern_stable) {
    /* Re-interning a meta-key name returns the same pointer. */
    CdoString *idx = cdo_string_intern("__index", 7);
    EXPECT_EQ(idx, g_meta_index);
    cdo_string_release(idx);

    CdoString *add = cdo_string_intern("__add", 5);
    EXPECT_EQ(add, g_meta_add);
    cdo_string_release(add);
}

/* -----------------------------------------------------------------------
 * Setting meta-keys on objects
 * --------------------------------------------------------------------- */
TEST(test_set_meta_index) {
    CdoObject *parent = cdo_object_new();
    CdoObject *child  = cdo_object_new();

    CdoValue pv = { .tag = CDO_OBJECT, .as = { .object = parent } };
    EXPECT_TRUE(cdo_object_rawset(child, g_meta_index, pv, FIELD_NONE));

    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(child, g_meta_index, &out));
    EXPECT_TRUE(cdo_is_any_object(out));
    EXPECT_EQ(out.as.object, parent);

    cdo_object_destroy(child);
    cdo_object_destroy(parent);
}

TEST(test_set_meta_type) {
    CdoObject *obj  = cdo_object_new();
    CdoString *name = cdo_string_intern("Widget", 6);
    CdoValue   tv   = cdo_string_value(cdo_string_retain(name));

    EXPECT_TRUE(cdo_object_rawset(obj, g_meta_type, tv, FIELD_STATIC));
    cdo_value_release(tv);

    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(obj, g_meta_type, &out));
    EXPECT_TRUE(cdo_is_string(out));
    EXPECT_STR(out.as.string->data, "Widget");

    /* FIELD_STATIC: second write must fail. */
    CdoValue tv2 = cdo_string_value(cdo_string_retain(name));
    EXPECT_FALSE(cdo_object_rawset(obj, g_meta_type, tv2, FIELD_NONE));
    cdo_value_release(tv2);

    cdo_string_release(name);
    cdo_object_destroy(obj);
}

TEST(test_set_meta_tostring) {
    CdoObject *obj    = cdo_object_new();
    CdoString *marker = cdo_string_intern("custom_str", 10);
    CdoValue   sv     = cdo_string_value(cdo_string_retain(marker));

    EXPECT_TRUE(cdo_object_rawset(obj, g_meta_tostring, sv, FIELD_NONE));
    cdo_value_release(sv);

    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(obj, g_meta_tostring, &out));
    EXPECT_TRUE(cdo_is_string(out));
    EXPECT_STR(out.as.string->data, "custom_str");

    cdo_string_release(marker);
    cdo_object_destroy(obj);
}

TEST(test_set_meta_eq) {
    CdoObject *obj = cdo_object_new();
    CdoValue   fv  = cdo_number(1.0);

    EXPECT_TRUE(cdo_object_rawset(obj, g_meta_eq, fv, FIELD_NONE));

    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(obj, g_meta_eq, &out));
    EXPECT_TRUE(cdo_is_number(out));
    EXPECT_EQ(out.as.number, 1.0);

    cdo_object_destroy(obj);
}

TEST(test_set_meta_lt) {
    CdoObject *obj = cdo_object_new();
    CdoValue   fv  = cdo_number(1.0);
    EXPECT_TRUE(cdo_object_rawset(obj, g_meta_lt, fv, FIELD_NONE));

    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(obj, g_meta_lt, &out));
    EXPECT_TRUE(cdo_is_number(out));
    cdo_object_destroy(obj);
}

TEST(test_set_meta_le) {
    CdoObject *obj = cdo_object_new();
    CdoValue   fv  = cdo_number(1.0);
    EXPECT_TRUE(cdo_object_rawset(obj, g_meta_le, fv, FIELD_NONE));

    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(obj, g_meta_le, &out));
    EXPECT_TRUE(cdo_is_number(out));
    cdo_object_destroy(obj);
}

TEST(test_set_meta_sub) {
    CdoObject *obj = cdo_object_new();
    CdoValue   fv  = cdo_number(1.0);
    EXPECT_TRUE(cdo_object_rawset(obj, g_meta_sub, fv, FIELD_NONE));

    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(obj, g_meta_sub, &out));
    EXPECT_TRUE(cdo_is_number(out));
    cdo_object_destroy(obj);
}

TEST(test_set_meta_mul) {
    CdoObject *obj = cdo_object_new();
    CdoValue   fv  = cdo_number(1.0);
    EXPECT_TRUE(cdo_object_rawset(obj, g_meta_mul, fv, FIELD_NONE));

    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(obj, g_meta_mul, &out));
    EXPECT_TRUE(cdo_is_number(out));
    cdo_object_destroy(obj);
}

TEST(test_set_meta_div) {
    CdoObject *obj = cdo_object_new();
    CdoValue   fv  = cdo_number(1.0);
    EXPECT_TRUE(cdo_object_rawset(obj, g_meta_div, fv, FIELD_NONE));

    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(obj, g_meta_div, &out));
    EXPECT_TRUE(cdo_is_number(out));
    cdo_object_destroy(obj);
}

TEST(test_set_meta_mod) {
    CdoObject *obj = cdo_object_new();
    CdoValue   fv  = cdo_number(1.0);
    EXPECT_TRUE(cdo_object_rawset(obj, g_meta_mod, fv, FIELD_NONE));

    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(obj, g_meta_mod, &out));
    EXPECT_TRUE(cdo_is_number(out));
    cdo_object_destroy(obj);
}

TEST(test_set_meta_pow) {
    CdoObject *obj = cdo_object_new();
    CdoValue   fv  = cdo_number(1.0);
    EXPECT_TRUE(cdo_object_rawset(obj, g_meta_pow, fv, FIELD_NONE));

    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(obj, g_meta_pow, &out));
    EXPECT_TRUE(cdo_is_number(out));
    cdo_object_destroy(obj);
}

TEST(test_set_meta_unm) {
    CdoObject *obj = cdo_object_new();
    CdoValue   fv  = cdo_number(5.0);
    EXPECT_TRUE(cdo_object_rawset(obj, g_meta_unm, fv, FIELD_NONE));

    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(obj, g_meta_unm, &out));
    EXPECT_TRUE(cdo_is_number(out));
    cdo_object_destroy(obj);
}

TEST(test_set_meta_idiv) {
    CdoObject *obj = cdo_object_new();
    CdoValue   fv  = cdo_number(1.0);
    EXPECT_TRUE(cdo_object_rawset(obj, g_meta_idiv, fv, FIELD_NONE));

    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(obj, g_meta_idiv, &out));
    EXPECT_TRUE(cdo_is_number(out));
    cdo_object_destroy(obj);
}

TEST(test_set_meta_len) {
    CdoObject *obj = cdo_object_new();
    CdoValue   fv  = cdo_number(2.0);

    EXPECT_TRUE(cdo_object_rawset(obj, g_meta_len, fv, FIELD_NONE));

    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(obj, g_meta_len, &out));
    EXPECT_TRUE(cdo_is_number(out));

    cdo_object_destroy(obj);
}

TEST(test_set_meta_add) {
    CdoObject *obj = cdo_object_new();
    CdoValue   fv  = cdo_number(3.0);
    EXPECT_TRUE(cdo_object_rawset(obj, g_meta_add, fv, FIELD_NONE));

    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(obj, g_meta_add, &out));
    EXPECT_TRUE(cdo_is_number(out));

    cdo_object_destroy(obj);
}

TEST(test_set_meta_newindex) {
    CdoObject *obj = cdo_object_new();
    CdoValue   fv  = cdo_number(4.0);
    EXPECT_TRUE(cdo_object_rawset(obj, g_meta_newindex, fv, FIELD_NONE));

    CdoValue out;
    EXPECT_TRUE(cdo_object_rawget(obj, g_meta_newindex, &out));
    EXPECT_TRUE(cdo_is_number(out));

    cdo_object_destroy(obj);
}


/* -----------------------------------------------------------------------
 * Class construction (cdo_class_new) tests
 * --------------------------------------------------------------------- */
TEST(test_class_new_anonymous) {
    CdoObject *cls = cdo_class_new(NULL, NULL);
    EXPECT_TRUE(cls != NULL);

    /* No __type and no __index when both args are NULL. */
    CdoValue out;
    EXPECT_FALSE(cdo_object_rawget(cls, g_meta_type, &out));
    EXPECT_FALSE(cdo_object_rawget(cls, g_meta_index, &out));

    cdo_object_destroy(cls);
}

TEST(test_class_new_with_type_only) {
    CdoString *name = cdo_string_intern("Foo", 3);
    CdoObject *cls  = cdo_class_new(name, NULL);

    CdoValue type_val;
    EXPECT_TRUE(cdo_object_rawget(cls, g_meta_type, &type_val));
    EXPECT_STR(type_val.as.string->data, "Foo");

    /* __index must not be set. */
    CdoValue idx_val;
    EXPECT_FALSE(cdo_object_rawget(cls, g_meta_index, &idx_val));

    cdo_string_release(name);
    cdo_object_destroy(cls);
}

TEST(test_class_new_with_proto) {
    CdoString *name  = cdo_string_intern("Bar", 3);
    CdoObject *proto = cdo_object_new();
    CdoString *mk    = cdo_string_intern("method", 6);
    cdo_object_rawset(proto, mk, cdo_number(42.0), FIELD_NONE);

    CdoObject *cls = cdo_class_new(name, proto);

    /* __index points to proto. */
    CdoValue idx_val;
    EXPECT_TRUE(cdo_object_rawget(cls, g_meta_index, &idx_val));
    EXPECT_EQ(idx_val.as.object, proto);

    /* Prototype-chain lookup finds the method through __index. */
    CdoValue found;
    EXPECT_TRUE(cdo_object_get(cls, mk, &found));
    EXPECT_EQ(found.as.number, 42.0);

    cdo_string_release(name);
    cdo_string_release(mk);
    cdo_object_destroy(cls);
    cdo_object_destroy(proto);
}

TEST(test_class_inheritance_two_levels) {
    /* Simulate: Child extends Parent, instance looks up via two __index hops. */
    CdoString *parent_name = cdo_string_intern("Parent", 6);
    CdoString *child_name  = cdo_string_intern("Child", 5);
    CdoString *mkey        = cdo_string_intern("shared", 6);

    CdoObject *parent_proto = cdo_object_new();
    cdo_object_rawset(parent_proto, mkey, cdo_number(7.0), FIELD_NONE);

    CdoObject *parent = cdo_class_new(parent_name, parent_proto);
    CdoObject *child  = cdo_class_new(child_name, parent);

    /* Instance: __index -> child -> parent -> parent_proto */
    CdoObject *inst = cdo_object_new();
    CdoValue   iv   = { .tag = CDO_OBJECT, .as = { .object = child } };
    cdo_object_rawset(inst, g_meta_index, iv, FIELD_NONE);

    CdoValue found;
    EXPECT_TRUE(cdo_object_get(inst, mkey, &found));
    EXPECT_EQ(found.as.number, 7.0);

    /* __type of child is "Child". */
    CdoValue child_type;
    EXPECT_TRUE(cdo_object_rawget(child, g_meta_type, &child_type));
    EXPECT_STR(child_type.as.string->data, "Child");

    cdo_string_release(parent_name);
    cdo_string_release(child_name);
    cdo_string_release(mkey);
    cdo_object_destroy(inst);
    cdo_object_destroy(child);
    cdo_object_destroy(parent);
    cdo_object_destroy(parent_proto);
}

TEST(test_prototype_chain_max_depth) {
    /* Chain of CANDO_PROTO_DEPTH_MAX objects; key is on the last one.
     * Lookup should succeed right at the limit. */
    const int  N   = CANDO_PROTO_DEPTH_MAX - 1; /* 31 links */
    CdoObject *objs[CANDO_PROTO_DEPTH_MAX];
    CdoString *k   = cdo_string_intern("deep_key", 8);

    for (int i = 0; i < CANDO_PROTO_DEPTH_MAX; i++)
        objs[i] = cdo_object_new();

    /* objs[0] is the instance; objs[N] holds the value. */
    cdo_object_rawset(objs[N], k, cdo_number(99.0), FIELD_NONE);
    for (int i = 0; i < N; i++) {
        CdoValue nv = { .tag = CDO_OBJECT, .as = { .object = objs[i+1] } };
        cdo_object_rawset(objs[i], g_meta_index, nv, FIELD_NONE);
    }

    CdoValue out;
    EXPECT_TRUE(cdo_object_get(objs[0], k, &out));
    EXPECT_EQ(out.as.number, 99.0);

    cdo_string_release(k);
    for (int i = 0; i < CANDO_PROTO_DEPTH_MAX; i++)
        cdo_object_destroy(objs[i]);
}

TEST(test_prototype_chain_beyond_max_depth) {
    /* Chain of CANDO_PROTO_DEPTH_MAX + 1 objects; the value is beyond the
     * depth limit so lookup must fail gracefully (no crash). */
    const int  N    = CANDO_PROTO_DEPTH_MAX + 1;
    CdoObject **objs = cando_alloc((u32)(sizeof(CdoObject *) * (u32)N));
    CdoString  *k    = cdo_string_intern("too_deep", 8);

    for (int i = 0; i < N; i++)
        objs[i] = cdo_object_new();

    cdo_object_rawset(objs[N-1], k, cdo_number(1.0), FIELD_NONE);
    for (int i = 0; i < N - 1; i++) {
        CdoValue nv = { .tag = CDO_OBJECT, .as = { .object = objs[i+1] } };
        cdo_object_rawset(objs[i], g_meta_index, nv, FIELD_NONE);
    }

    CdoValue out;
    EXPECT_FALSE(cdo_object_get(objs[0], k, &out));

    cdo_string_release(k);
    for (int i = 0; i < N; i++)
        cdo_object_destroy(objs[i]);
    cando_free(objs);
}

/* -----------------------------------------------------------------------
 * Thread-safety smoke test: concurrent reads/writes on shared object
 * --------------------------------------------------------------------- */
#define MT_THREADS 4
#define MT_ITERS   1000

typedef struct {
    CdoObject  *obj;
    CdoString  *key;
    int         iters;
} MTArg;

static void *mt_writer(void *arg) {
    MTArg *a = arg;
    for (int i = 0; i < a->iters; i++)
        cdo_object_rawset(a->obj, a->key, cdo_number((f64)i), FIELD_NONE);
    return NULL;
}

static void *mt_reader(void *arg) {
    MTArg *a = arg;
    for (int i = 0; i < a->iters; i++) {
        CdoValue v;
        cdo_object_rawget(a->obj, a->key, &v);
    }
    return NULL;
}

TEST(test_concurrent_read_write) {
    CdoObject *obj = cdo_object_new();
    CdoString *key = cdo_string_intern("counter", 7);
    cdo_object_rawset(obj, key, cdo_number(0.0), FIELD_NONE);

    MTArg arg = { obj, key, MT_ITERS };
    pthread_t writers[MT_THREADS], readers[MT_THREADS];

    for (int i = 0; i < MT_THREADS; i++) {
        pthread_create(&writers[i], NULL, mt_writer, &arg);
        pthread_create(&readers[i], NULL, mt_reader, &arg);
    }
    for (int i = 0; i < MT_THREADS; i++) {
        pthread_join(writers[i], NULL);
        pthread_join(readers[i], NULL);
    }

    /* Just verify the field is still accessible and has a number value. */
    CdoValue v;
    EXPECT_TRUE(cdo_object_rawget(obj, key, &v));
    EXPECT_TRUE(cdo_is_number(v));

    cdo_string_release(key);
    cdo_object_destroy(obj);
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void) {
    printf("\n=== Cando object system unit tests ===\n\n");

    cdo_object_init();

    printf("-- CdoString --\n");
    run_test("string new/release",        test_string_new_release);
    run_test("string retain",             test_string_retain);
    run_test("string hash",               test_string_hash);

    printf("\n-- String interning --\n");
    run_test("intern same pointer",       test_intern_same_pointer);
    run_test("intern different content",  test_intern_different_content);
    run_test("intern ref count",          test_intern_ref_count);

    printf("\n-- CdoValue --\n");
    run_test("value null",                test_value_null);
    run_test("value bool",                test_value_bool);
    run_test("value number",              test_value_number);
    run_test("value string",              test_value_string);
    run_test("value equal",               test_value_equal);
    run_test("value copy/release",        test_value_copy_release);

    printf("\n-- CdoObject (plain) --\n");
    run_test("object new/destroy",        test_object_new_destroy);
    run_test("rawset / rawget",           test_object_rawset_rawget);
    run_test("rawget missing",            test_object_rawget_missing);
    run_test("rawset update",             test_object_rawset_update);
    run_test("rawdelete",                 test_object_rawdelete);
    run_test("readonly",                  test_object_readonly);

    printf("\n-- Field flags --\n");
    run_test("FIELD_STATIC",              test_field_static);
    run_test("FIELD_PRIVATE stored",      test_field_private_flag_stored);

    printf("\n-- FIFO ordering --\n");
    run_test("insertion order",           test_fifo_insertion_order);
    run_test("order after delete",        test_fifo_after_delete);
    run_test("many fields (rehash)",      test_many_fields);

    printf("\n-- Array --\n");
    run_test("array new",                 test_array_new);
    run_test("array push/get",            test_array_push_get);
    run_test("array set by index",        test_array_set_idx);
    run_test("array readonly",            test_array_readonly);

    printf("\n-- Array (extended) --\n");
    run_test("insert middle",             test_array_insert_middle);
    run_test("insert at end",             test_array_insert_end);
    run_test("insert beyond len (clamp)", test_array_insert_beyond_len);
    run_test("remove middle",             test_array_remove_middle);
    run_test("remove first",              test_array_remove_first);
    run_test("remove out-of-bounds",      test_array_remove_oob);
    run_test("remove readonly",           test_array_remove_readonly);

    printf("\n-- Function / Native --\n");
    run_test("function new",              test_function_new);
    run_test("function upvalues",         test_function_upvalues);
    run_test("native new",                test_native_new);

    printf("\n-- Class --\n");
    run_test("class new",                 test_class_new);

    printf("\n-- Prototype chain --\n");
    run_test("basic __index chain",       test_prototype_chain_basic);
    run_test("3-level chain",             test_prototype_chain_depth);
    run_test("self-loop guard",           test_prototype_self_loop_guard);

    printf("\n-- Length --\n");
    run_test("object length",             test_object_length);
    run_test("array length",              test_array_length);

    printf("\n-- Meta-key constants --\n");
    run_test("meta-keys interned",        test_meta_keys_interned);
    run_test("meta-keys content",         test_meta_keys_content);
    run_test("meta-keys unique pointers", test_meta_keys_unique_pointers);
    run_test("meta-keys intern stable",   test_meta_keys_intern_stable);

    printf("\n-- Setting meta-keys on objects --\n");
    run_test("set __index",               test_set_meta_index);
    run_test("set __type (static)",       test_set_meta_type);
    run_test("set __tostring",            test_set_meta_tostring);
    run_test("set __eq",                  test_set_meta_eq);
    run_test("set __lt",                  test_set_meta_lt);
    run_test("set __le",                  test_set_meta_le);
    run_test("set __len",                 test_set_meta_len);
    run_test("set __add",                 test_set_meta_add);
    run_test("set __sub",                 test_set_meta_sub);
    run_test("set __mul",                 test_set_meta_mul);
    run_test("set __div",                 test_set_meta_div);
    run_test("set __mod",                 test_set_meta_mod);
    run_test("set __pow",                 test_set_meta_pow);
    run_test("set __unm",                 test_set_meta_unm);
    run_test("set __idiv",                test_set_meta_idiv);
    run_test("set __newindex",            test_set_meta_newindex);

    printf("\n-- Class construction (cdo_class_new) --\n");
    run_test("class_new anonymous",       test_class_new_anonymous);
    run_test("class_new type only",       test_class_new_with_type_only);
    run_test("class_new with proto",      test_class_new_with_proto);
    run_test("class inheritance 2-level", test_class_inheritance_two_levels);
    run_test("proto chain max depth",     test_prototype_chain_max_depth);
    run_test("proto chain beyond depth",  test_prototype_chain_beyond_max_depth);

    printf("\n-- Thread safety --\n");
    run_test("concurrent read/write",     test_concurrent_read_write);

    cdo_object_destroy_globals();

    printf("\n=== Results: %d/%d passed ===\n", g_passed, g_run);
    return g_failed > 0 ? 1 : 0;
}
