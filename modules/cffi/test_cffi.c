/*
 * modules/cffi/test_cffi.c -- C unit tests for the cffi module's
 * pure-C helpers.
 *
 * Built and run with:
 *
 *     make -C modules/cffi test
 *
 * These tests do not link against libcando -- they exercise the
 * pieces that don't touch the VM: the type-string parser, the kind
 * width table, the platform-dependent C-type mappings, and (linked in
 * separately) a small libffi smoke that confirms the dependency is
 * actually present and callable.  Script-level coverage of the module
 * surface lives in test_cffi.cdo.
 */

#include "cffi_types.h"

#include <ffi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int failures = 0;

#define EXPECT(name, cond) do {                            \
    if (cond) {                                            \
        printf("  PASS  %s\n", name);                      \
    } else {                                               \
        printf("  FAIL  %s\n", name);                      \
        failures++;                                        \
    }                                                      \
} while (0)

#define EXPECT_EQ(name, a, b) do {                                     \
    long long _a = (long long)(a), _b = (long long)(b);                \
    if (_a == _b) {                                                    \
        printf("  PASS  %s\n", name);                                  \
    } else {                                                           \
        printf("  FAIL  %s  (%lld != %lld)\n", name, _a, _b);          \
        failures++;                                                    \
    }                                                                  \
} while (0)

#define EXPECT_STREQ(name, a, b) do {                                  \
    const char *_a = (a), *_b = (b);                                   \
    if (_a && _b && strcmp(_a, _b) == 0) {                             \
        printf("  PASS  %s\n", name);                                  \
    } else {                                                           \
        printf("  FAIL  %s  (\"%s\" != \"%s\")\n", name,               \
               _a ? _a : "(null)", _b ? _b : "(null)");                \
        failures++;                                                    \
    }                                                                  \
} while (0)

/* ---------------------------------------------------------------- *
 * Compact helper: parse a signature and check it has the expected
 * return kind and arg list.
 * ---------------------------------------------------------------- */

static int sig_check(const char *text, CffiKind ret, int nargs,
                     const CffiKind *args, CffiSig *out_sig)
{
    CffiParseError err;
    CffiSig sig;
    if (!cffi_parse_signature(text, &sig, &err)) {
        printf("    parse failed @ %zu: %s\n", err.offset, err.message);
        return 0;
    }
    if (sig.ret != ret)        return 0;
    if (sig.nargs != nargs)    return 0;
    for (int i = 0; i < nargs; i++) {
        if (sig.args[i] != args[i]) return 0;
    }
    if (out_sig) *out_sig = sig;
    return 1;
}

/* ===================================================================
 * kind size / align table
 * =================================================================== */

static void test_kind_table(void)
{
    printf("\n[kind_table]\n");
    EXPECT_EQ("sizeof(void)   == 0",     cffi_kind_size(CFFI_VOID), 0);
    EXPECT_EQ("sizeof(bool)   == 1",     cffi_kind_size(CFFI_BOOL), 1);
    EXPECT_EQ("sizeof(int8)   == 1",     cffi_kind_size(CFFI_I8),   1);
    EXPECT_EQ("sizeof(uint8)  == 1",     cffi_kind_size(CFFI_U8),   1);
    EXPECT_EQ("sizeof(int16)  == 2",     cffi_kind_size(CFFI_I16),  2);
    EXPECT_EQ("sizeof(uint16) == 2",     cffi_kind_size(CFFI_U16),  2);
    EXPECT_EQ("sizeof(int32)  == 4",     cffi_kind_size(CFFI_I32),  4);
    EXPECT_EQ("sizeof(uint32) == 4",     cffi_kind_size(CFFI_U32),  4);
    EXPECT_EQ("sizeof(int64)  == 8",     cffi_kind_size(CFFI_I64),  8);
    EXPECT_EQ("sizeof(uint64) == 8",     cffi_kind_size(CFFI_U64),  8);
    EXPECT_EQ("sizeof(float)  == 4",     cffi_kind_size(CFFI_F32),  4);
    EXPECT_EQ("sizeof(double) == 8",     cffi_kind_size(CFFI_F64),  8);
    EXPECT_EQ("sizeof(ptr)    == sizeof(void*)",
              cffi_kind_size(CFFI_PTR), (long long)sizeof(void*));
    EXPECT_EQ("sizeof(cstr)   == sizeof(void*)",
              cffi_kind_size(CFFI_CSTR), (long long)sizeof(void*));

    EXPECT_EQ("align(int32) == 4",  cffi_kind_align(CFFI_I32), 4);
    EXPECT_EQ("align(int64) == 8",  cffi_kind_align(CFFI_I64), 8);
    EXPECT_EQ("align(ptr)   == sizeof(void*)",
              cffi_kind_align(CFFI_PTR), (long long)sizeof(void*));

    EXPECT("integer(uint16)",    cffi_kind_is_integer(CFFI_U16));
    EXPECT("integer(int64)",     cffi_kind_is_integer(CFFI_I64));
    EXPECT("integer(bool)",      cffi_kind_is_integer(CFFI_BOOL));
    EXPECT("!integer(float)",   !cffi_kind_is_integer(CFFI_F32));
    EXPECT("!integer(ptr)",     !cffi_kind_is_integer(CFFI_PTR));
    EXPECT("!integer(void)",    !cffi_kind_is_integer(CFFI_VOID));

    EXPECT("signed(int32)",      cffi_kind_is_signed(CFFI_I32));
    EXPECT("!signed(uint32)",   !cffi_kind_is_signed(CFFI_U32));
    EXPECT("!signed(bool)",     !cffi_kind_is_signed(CFFI_BOOL));

    EXPECT("float(F32)",         cffi_kind_is_float(CFFI_F32));
    EXPECT("float(F64)",         cffi_kind_is_float(CFFI_F64));
    EXPECT("!float(I64)",       !cffi_kind_is_float(CFFI_I64));

    EXPECT("pointer(PTR)",       cffi_kind_is_pointer(CFFI_PTR));
    EXPECT("pointer(CSTR)",      cffi_kind_is_pointer(CFFI_CSTR));
    EXPECT("!pointer(I64)",     !cffi_kind_is_pointer(CFFI_I64));

    EXPECT_STREQ("name(int32)",  cffi_kind_name(CFFI_I32),  "int32");
    EXPECT_STREQ("name(uint64)", cffi_kind_name(CFFI_U64),  "uint64");
    EXPECT_STREQ("name(ptr)",    cffi_kind_name(CFFI_PTR),  "pointer");
    EXPECT_STREQ("name(cstr)",   cffi_kind_name(CFFI_CSTR), "cstring");
    EXPECT_STREQ("name(void)",   cffi_kind_name(CFFI_VOID), "void");
}

/* ===================================================================
 * Platform-dependent type kinds (long, size_t, etc.)
 * =================================================================== */

static void test_platform_kinds(void)
{
    printf("\n[platform_kinds]\n");
    EXPECT_EQ("CFFI_LONG_KIND  size == sizeof(long)",
              cffi_kind_size(CFFI_LONG_KIND),  (long long)sizeof(long));
    EXPECT_EQ("CFFI_ULONG_KIND size == sizeof(unsigned long)",
              cffi_kind_size(CFFI_ULONG_KIND), (long long)sizeof(unsigned long));
    EXPECT_EQ("CFFI_SIZE_KIND  size == sizeof(size_t)",
              cffi_kind_size(CFFI_SIZE_KIND),  (long long)sizeof(size_t));
    EXPECT_EQ("CFFI_SSIZE_KIND size == sizeof(size_t)",
              cffi_kind_size(CFFI_SSIZE_KIND), (long long)sizeof(size_t));
    EXPECT_EQ("CFFI_INTPTR_KIND  size == sizeof(intptr_t)",
              cffi_kind_size(CFFI_INTPTR_KIND),  (long long)sizeof(intptr_t));
    EXPECT_EQ("CFFI_UINTPTR_KIND size == sizeof(uintptr_t)",
              cffi_kind_size(CFFI_UINTPTR_KIND), (long long)sizeof(uintptr_t));
    EXPECT_EQ("CFFI_LLONG_KIND  size == sizeof(long long)",
              cffi_kind_size(CFFI_LLONG_KIND),   (long long)sizeof(long long));

    EXPECT("CFFI_SSIZE_KIND is signed",
           cffi_kind_is_signed(CFFI_SSIZE_KIND));
    EXPECT("CFFI_SIZE_KIND is unsigned",
          !cffi_kind_is_signed(CFFI_SIZE_KIND));
}

/* ===================================================================
 * Single-type parser
 * =================================================================== */

static int parse_one(const char *text, CffiKind *out)
{
    CffiParseError err;
    return cffi_parse_single_type(text, out, &err);
}

#define EXPECT_TYPE(text, expected) do {                          \
    CffiKind _k = (CffiKind)-1;                                   \
    int _ok = parse_one((text), &_k);                             \
    if (_ok && _k == (expected)) {                                \
        printf("  PASS  parse \"%s\" -> %s\n", (text),            \
               cffi_kind_name(expected));                         \
    } else {                                                      \
        printf("  FAIL  parse \"%s\" -> %s (got %s%s)\n", (text), \
               cffi_kind_name(expected),                          \
               _ok ? cffi_kind_name(_k) : "parse-error",          \
               _ok ? "" : "");                                    \
        failures++;                                               \
    }                                                             \
} while (0)

static void test_parse_primitives(void)
{
    printf("\n[parse_primitives]\n");

    EXPECT_TYPE("void",   CFFI_VOID);
    EXPECT_TYPE("bool",   CFFI_BOOL);
    EXPECT_TYPE("_Bool",  CFFI_BOOL);

    EXPECT_TYPE("char",          CFFI_I8);
    EXPECT_TYPE("signed char",   CFFI_I8);
    EXPECT_TYPE("unsigned char", CFFI_U8);

    EXPECT_TYPE("short",              CFFI_I16);
    EXPECT_TYPE("short int",          CFFI_I16);
    EXPECT_TYPE("signed short",       CFFI_I16);
    EXPECT_TYPE("signed short int",   CFFI_I16);
    EXPECT_TYPE("unsigned short",     CFFI_U16);
    EXPECT_TYPE("unsigned short int", CFFI_U16);

    EXPECT_TYPE("int",           CFFI_I32);
    EXPECT_TYPE("signed",        CFFI_I32);
    EXPECT_TYPE("signed int",    CFFI_I32);
    EXPECT_TYPE("unsigned",      CFFI_U32);
    EXPECT_TYPE("unsigned int",  CFFI_U32);

    EXPECT_TYPE("long",              CFFI_LONG_KIND);
    EXPECT_TYPE("long int",          CFFI_LONG_KIND);
    EXPECT_TYPE("signed long",       CFFI_LONG_KIND);
    EXPECT_TYPE("signed long int",   CFFI_LONG_KIND);
    EXPECT_TYPE("unsigned long",     CFFI_ULONG_KIND);
    EXPECT_TYPE("unsigned long int", CFFI_ULONG_KIND);

    EXPECT_TYPE("long long",              CFFI_I64);
    EXPECT_TYPE("long long int",          CFFI_I64);
    EXPECT_TYPE("signed long long",       CFFI_I64);
    EXPECT_TYPE("unsigned long long",     CFFI_U64);
    EXPECT_TYPE("unsigned long long int", CFFI_U64);

    EXPECT_TYPE("float",  CFFI_F32);
    EXPECT_TYPE("double", CFFI_F64);

    EXPECT_TYPE("int8_t",   CFFI_I8);
    EXPECT_TYPE("int16_t",  CFFI_I16);
    EXPECT_TYPE("int32_t",  CFFI_I32);
    EXPECT_TYPE("int64_t",  CFFI_I64);
    EXPECT_TYPE("uint8_t",  CFFI_U8);
    EXPECT_TYPE("uint16_t", CFFI_U16);
    EXPECT_TYPE("uint32_t", CFFI_U32);
    EXPECT_TYPE("uint64_t", CFFI_U64);

    EXPECT_TYPE("size_t",    CFFI_SIZE_KIND);
    EXPECT_TYPE("ssize_t",   CFFI_SSIZE_KIND);
    EXPECT_TYPE("ptrdiff_t", CFFI_PTRDIFF_KIND);
    EXPECT_TYPE("intptr_t",  CFFI_INTPTR_KIND);
    EXPECT_TYPE("uintptr_t", CFFI_UINTPTR_KIND);
}

static void test_parse_pointers(void)
{
    printf("\n[parse_pointers]\n");

    /* char* / const char* should become CFFI_CSTR. */
    EXPECT_TYPE("char*",                CFFI_CSTR);
    EXPECT_TYPE("char *",               CFFI_CSTR);
    EXPECT_TYPE("const char*",          CFFI_CSTR);
    EXPECT_TYPE("const char *",         CFFI_CSTR);
    EXPECT_TYPE("char const *",         CFFI_CSTR);
    EXPECT_TYPE("const  char  *",       CFFI_CSTR);

    /* Other pointers are opaque. */
    EXPECT_TYPE("void*",                CFFI_PTR);
    EXPECT_TYPE("void *",               CFFI_PTR);
    EXPECT_TYPE("int*",                 CFFI_PTR);
    EXPECT_TYPE("const int*",           CFFI_PTR);
    EXPECT_TYPE("unsigned char*",       CFFI_PTR);
    EXPECT_TYPE("float*",               CFFI_PTR);
    EXPECT_TYPE("double*",              CFFI_PTR);
    EXPECT_TYPE("size_t*",              CFFI_PTR);
    EXPECT_TYPE("uint8_t*",             CFFI_PTR);

    /* Double-pointer collapses to opaque, including char** (a buffer
     * of pointers, not an array of C strings). */
    EXPECT_TYPE("char**",               CFFI_PTR);
    EXPECT_TYPE("const char**",         CFFI_PTR);
    EXPECT_TYPE("void**",               CFFI_PTR);
    EXPECT_TYPE("int***",               CFFI_PTR);

    /* Qualifiers between the type and the star, around the star, etc. */
    EXPECT_TYPE("int * const",          CFFI_PTR);
    EXPECT_TYPE("const int * const",    CFFI_PTR);
    EXPECT_TYPE("volatile void *",      CFFI_PTR);
    /* `restrict char*` -- the qualifier sits before `char`, so the type
     * is still `char*` and we treat it as CSTR.  Real C grammar puts
     * `restrict` between the type and the star, but accepting either
     * order keeps copy-pasted-from-headers signatures parseable. */
    EXPECT_TYPE("restrict char *",      CFFI_CSTR);
    EXPECT_TYPE("char * restrict",      CFFI_CSTR);
    EXPECT_TYPE("int * restrict",       CFFI_PTR);

    /* Whitespace tolerance. */
    EXPECT_TYPE("   int   ",            CFFI_I32);
    EXPECT_TYPE("\tdouble\n",           CFFI_F64);
}

static void test_parse_signatures(void)
{
    printf("\n[parse_signatures]\n");

    CffiKind args[CFFI_MAX_ARGS];

    /* No-arg variants. */
    EXPECT("int()             parses",  sig_check("int()",     CFFI_I32,  0, NULL, NULL));
    EXPECT("int(void)         parses",  sig_check("int(void)", CFFI_I32,  0, NULL, NULL));
    EXPECT("void()            parses",  sig_check("void()",    CFFI_VOID, 0, NULL, NULL));
    EXPECT("void(void)        parses",  sig_check("void(void)",CFFI_VOID, 0, NULL, NULL));

    /* Single-arg variants. */
    args[0] = CFFI_I32;
    EXPECT("int(int)          parses",  sig_check("int(int)", CFFI_I32, 1, args, NULL));

    args[0] = CFFI_CSTR;
    EXPECT("size_t(const char*) parses",
           sig_check("size_t(const char*)", CFFI_SIZE_KIND, 1, args, NULL));

    args[0] = CFFI_PTR;
    EXPECT("void(void*)       parses",  sig_check("void(void*)", CFFI_VOID, 1, args, NULL));

    /* Multi-arg with mixed kinds. */
    args[0] = CFFI_CSTR; args[1] = CFFI_CSTR;
    EXPECT("int(const char*, const char*) parses",
           sig_check("int(const char*, const char*)", CFFI_I32, 2, args, NULL));

    args[0] = CFFI_PTR; args[1] = CFFI_PTR; args[2] = CFFI_SIZE_KIND;
    EXPECT("void*(void*, const void*, size_t) parses",
           sig_check("void*(void*, const void*, size_t)", CFFI_PTR, 3, args, NULL));

    /* Tolerance to surrounding whitespace. */
    args[0] = CFFI_I32; args[1] = CFFI_I32;
    EXPECT("  int  ( int , int )  parses",
           sig_check("  int  ( int , int )  ", CFFI_I32, 2, args, NULL));

    /* Pointer return -- char* on both sides marshals as CSTR. */
    args[0] = CFFI_CSTR;
    EXPECT("char*(const char*)  parses",
           sig_check("char*(const char*)", CFFI_CSTR, 1, args, NULL));
    EXPECT("void*(const void*)  parses",
           sig_check("void*(const void*)", CFFI_PTR,
                     1, (CffiKind[]){ CFFI_PTR }, NULL));

    /* float / double round-trip. */
    args[0] = CFFI_F64;
    EXPECT("double(double)     parses",
           sig_check("double(double)", CFFI_F64, 1, args, NULL));
    args[0] = CFFI_F32;
    EXPECT("float(float)       parses",
           sig_check("float(float)",   CFFI_F32, 1, args, NULL));
}

static void test_parse_errors(void)
{
    printf("\n[parse_errors]\n");

    CffiParseError err;
    CffiSig sig;

    EXPECT("null input rejected",
           !cffi_parse_signature(NULL, &sig, &err));

    EXPECT("empty input rejected",
           !cffi_parse_signature("", &sig, &err));

    EXPECT("missing ( rejected",
           !cffi_parse_signature("int", &sig, &err));

    EXPECT("missing ) rejected",
           !cffi_parse_signature("int(", &sig, &err));

    EXPECT("garbage after ) rejected",
           !cffi_parse_signature("int(void) garbage", &sig, &err));

    EXPECT("bogus return type rejected",
           !cffi_parse_signature("frobnicate()", &sig, &err));

    EXPECT("bogus arg type rejected",
           !cffi_parse_signature("int(frobnicate)", &sig, &err));

    EXPECT("void parameter (not 'void')  rejected",
           !cffi_parse_signature("int(void, int)", &sig, &err));

    EXPECT("trailing comma rejected",
           !cffi_parse_signature("int(int,)", &sig, &err));

    EXPECT("missing arg between commas rejected",
           !cffi_parse_signature("int(,)", &sig, &err));

    EXPECT("long double rejected (unsupported)",
           !cffi_parse_signature("long double()", &sig, &err));

    /* Too many arguments. */
    {
        char big[256];
        size_t off = 0;
        memcpy(big + off, "void(", 5); off += 5;
        for (int i = 0; i < CFFI_MAX_ARGS + 1; i++) {
            if (i) big[off++] = ',';
            big[off++] = 'i'; big[off++] = 'n'; big[off++] = 't';
        }
        big[off++] = ')';
        big[off] = '\0';
        EXPECT("too-many-args rejected",
               !cffi_parse_signature(big, &sig, &err));
    }

    /* Error message should be populated, not empty. */
    cffi_parse_signature("int(frobnicate)", &sig, &err);
    EXPECT("err.message populated on failure", err.message[0] != '\0');
}

/* ===================================================================
 * libffi smoke -- prove the dependency is wired in and a no-arg
 * native function pointer can be called.
 * =================================================================== */

static int answer_to_everything(void) { return 42; }
static int double_it(int x)           { return x * 2; }

static void test_libffi_smoke(void)
{
    printf("\n[libffi_smoke]\n");

    /* No-arg int(). */
    {
        ffi_cif cif;
        ffi_type *atypes[1] = { NULL };
        ffi_status st = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 0,
                                     &ffi_type_sint, atypes);
        EXPECT("ffi_prep_cif (int())  == FFI_OK", st == FFI_OK);
        ffi_arg ret = 0;
        ffi_call(&cif, FFI_FN(answer_to_everything), &ret, NULL);
        EXPECT("ffi_call answer == 42", (int)ret == 42);
    }

    /* One-arg int(int). */
    {
        ffi_cif cif;
        ffi_type *atypes[1] = { &ffi_type_sint };
        ffi_status st = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1,
                                     &ffi_type_sint, atypes);
        EXPECT("ffi_prep_cif (int(int)) == FFI_OK", st == FFI_OK);
        int x = 21;
        void *args[1] = { &x };
        ffi_arg ret = 0;
        ffi_call(&cif, FFI_FN(double_it), &ret, args);
        EXPECT("ffi_call double_it(21) == 42", (int)ret == 42);
    }
}

/* ===================================================================
 * main
 * =================================================================== */

int main(void)
{
    printf("cffi module C unit tests\n");
    printf("------------------------\n");

    test_kind_table();
    test_platform_kinds();
    test_parse_primitives();
    test_parse_pointers();
    test_parse_signatures();
    test_parse_errors();
    test_libffi_smoke();

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall passed\n");
    return 0;
}
