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

    printf("\n-- Thread safety --\n");
    run_test("concurrent read/write",     test_concurrent_read_write);

    cdo_object_destroy_globals();

    printf("\n=== Results: %d/%d passed ===\n", g_passed, g_run);
    return g_failed > 0 ? 1 : 0;
}
