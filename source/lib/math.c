/*
 * lib/math.c -- Native math library for Cando.
 *
 * Must compile with gcc -std=c11.
 */

#include "math.h"
#include "libutil.h"
#include "../vm/bridge.h"
#include "../object/object.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * math.clamp(value, min, max) → number
 * ======================================================================= */

static int math_clamp(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc < 1 || !cando_is_number(args[0])) {
        cando_vm_push(vm, cando_number(0));
        return 1;
    }

    f64 value = libutil_arg_num(args[0], 0.0);

    if (argc >= 2 && cando_is_number(args[1])) {
        f64 mn = libutil_arg_num(args[1], 0.0);
        if (value < mn) value = mn;
    }

    if (argc >= 3 && cando_is_number(args[2])) {
        f64 mx = libutil_arg_num(args[2], 0.0);
        if (value > mx) value = mx;
    }

    cando_vm_push(vm, cando_number(value));
    return 1;
}

/* =========================================================================
 * math.min(...) → number
 * ======================================================================= */

static int math_min(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc == 0) {
        cando_vm_push(vm, cando_number(0));
        return 1;
    }

    f64 res = libutil_arg_num(args[0], 0.0);
    for (int i = 1; i < argc; i++) {
        f64 val = libutil_arg_num(args[i], 0.0);
        if (val < res) res = val;
    }

    cando_vm_push(vm, cando_number(res));
    return 1;
}

/* =========================================================================
 * math.max(...) → number
 * ======================================================================= */

static int math_max(CandoVM *vm, int argc, CandoValue *args)
{
    if (argc == 0) {
        cando_vm_push(vm, cando_number(0));
        return 1;
    }

    f64 res = libutil_arg_num(args[0], 0.0);
    for (int i = 1; i < argc; i++) {
        f64 val = libutil_arg_num(args[i], 0.0);
        if (val > res) res = val;
    }

    cando_vm_push(vm, cando_number(res));
    return 1;
}

/* =========================================================================
 * math.sin(n) → number
 * ======================================================================= */

static int math_sin(CandoVM *vm, int argc, CandoValue *args)
{
    f64 val = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(sin(val)));
    return 1;
}

/* =========================================================================
 * math.cos(n) → number
 * ======================================================================= */

static int math_cos(CandoVM *vm, int argc, CandoValue *args)
{
    f64 val = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(cos(val)));
    return 1;
}

/* =========================================================================
 * Trigonometry (Extended)
 * ======================================================================= */

static int math_tan(CandoVM *vm, int argc, CandoValue *args) {
    f64 val = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(tan(val)));
    return 1;
}

static int math_asin(CandoVM *vm, int argc, CandoValue *args) {
    f64 val = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(asin(val)));
    return 1;
}

static int math_acos(CandoVM *vm, int argc, CandoValue *args) {
    f64 val = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(acos(val)));
    return 1;
}

static int math_atan(CandoVM *vm, int argc, CandoValue *args) {
    f64 val = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(atan(val)));
    return 1;
}

/* math.atan2(y, x) → useful for getting angles between points */
static int math_atan2(CandoVM *vm, int argc, CandoValue *args) {
    f64 y = libutil_arg_num_at(args, argc, 0, 0.0);
    f64 x = libutil_arg_num_at(args, argc, 1, 0.0);
    cando_vm_push(vm, cando_number(atan2(y, x)));
    return 1;
}

/* =========================================================================
 * Conversion helpers
 * ======================================================================= */

static int math_rad(CandoVM *vm, int argc, CandoValue *args) {
    f64 deg = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(deg * (M_PI / 180.0)));
    return 1;
}

static int math_deg(CandoVM *vm, int argc, CandoValue *args) {
    f64 rad = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(rad * (180.0 / M_PI)));
    return 1;
}

/* =========================================================================
 * Hyperbolic / Sign
 * ======================================================================= */

static int math_sinh(CandoVM *vm, int argc, CandoValue *args) {
    f64 val = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(sinh(val)));
    return 1;
}

static int math_cosh(CandoVM *vm, int argc, CandoValue *args) {
    f64 val = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(cosh(val)));
    return 1;
}

static int math_sign(CandoVM *vm, int argc, CandoValue *args) {
    f64 val = libutil_arg_num_at(args, argc, 0, 0.0);
    if (val > 0)      cando_vm_push(vm, cando_number(1));
    else if (val < 0) cando_vm_push(vm, cando_number(-1));
    else              cando_vm_push(vm, cando_number(0));
    return 1;
}

/* =========================================================================
 * math.random(min, max) → number
 * math.random(max) → number
 * math.random() → number (0 to 1)
 * ======================================================================= */

static int math_random(CandoVM *vm, int argc, CandoValue *args) {
    f64 result;
    if (argc == 0) {
        result = (f64)rand() / (f64)RAND_MAX;
    } else if (argc == 1) {
        f64 mx = libutil_arg_num(args[0], 0.0);
        result = ((f64)rand() / (f64)RAND_MAX) * mx;
    } else {
        f64 mn = libutil_arg_num(args[0], 0.0);
        f64 mx = libutil_arg_num(args[1], 0.0);
        result = mn + ((f64)rand() / (f64)RAND_MAX) * (mx - mn);
    }
    cando_vm_push(vm, cando_number(result));
    return 1;
}

/* =========================================================================
 * math.round(n) / math.floor(n) / math.ceil(n)
 * ======================================================================= */

static int math_round(CandoVM *vm, int argc, CandoValue *args) {
    f64 val = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(round(val)));
    return 1;
}

static int math_floor(CandoVM *vm, int argc, CandoValue *args) {
    f64 val = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(floor(val)));
    return 1;
}

static int math_ceil(CandoVM *vm, int argc, CandoValue *args) {
    f64 val = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(ceil(val)));
    return 1;
}

/* =========================================================================
 * math.log(n, base?) → number
 * ======================================================================= */

static int math_log(CandoVM *vm, int argc, CandoValue *args) {
    if (argc == 0) {
        cando_vm_push(vm, cando_number(0));
        return 1;
    }
    f64 val = libutil_arg_num(args[0], 0.0);
    if (argc >= 2) {
        f64 base = libutil_arg_num(args[1], 0.0);
        cando_vm_push(vm, cando_number(log(val) / log(base)));
    } else {
        cando_vm_push(vm, cando_number(log(val)));
    }
    return 1;
}

static int math_log10(CandoVM *vm, int argc, CandoValue *args) {
    f64 val = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(log10(val)));
    return 1;
}

static int math_exp(CandoVM *vm, int argc, CandoValue *args) {
    f64 val = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(exp(val)));
    return 1;
}

/* =========================================================================
 * math.abs(n) / math.sqrt(n) / math.pow(base, exp)
 * ======================================================================= */

static int math_abs(CandoVM *vm, int argc, CandoValue *args) {
    f64 val = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(fabs(val)));
    return 1;
}

static int math_sqrt(CandoVM *vm, int argc, CandoValue *args) {
    f64 val = libutil_arg_num_at(args, argc, 0, 0.0);
    cando_vm_push(vm, cando_number(sqrt(val)));
    return 1;
}

static int math_pow(CandoVM *vm, int argc, CandoValue *args) {
    f64 base = libutil_arg_num_at(args, argc, 0, 0.0);
    f64 exp  = libutil_arg_num_at(args, argc, 1, 0.0);
    cando_vm_push(vm, cando_number(pow(base, exp)));
    return 1;
}

/* =========================================================================
 * Registration
 * ======================================================================= */

static void set_const(CdoObject *obj, const char *name, f64 value)
{
    CdoString *key = cdo_string_intern(name, (u32)strlen(name));
    cdo_object_rawset(obj, key, cdo_number(value), FIELD_NONE);
    cdo_string_release(key);
}

static const LibutilMethodEntry math_methods[] = {
    { "clamp",  math_clamp  },
    { "min",    math_min    },
    { "max",    math_max    },
    { "sin",    math_sin    },
    { "cos",    math_cos    },
    { "tan",    math_tan    },
    { "asin",   math_asin   },
    { "acos",   math_acos   },
    { "atan",   math_atan   },
    { "atan2",  math_atan2  },
    { "rad",    math_rad    },
    { "deg",    math_deg    },
    { "sign",   math_sign   },
    { "sinh",   math_sinh   },
    { "cosh",   math_cosh   },
    { "random", math_random },
    { "round",  math_round  },
    { "floor",  math_floor  },
    { "ceil",   math_ceil   },
    { "log",    math_log    },
    { "log10",  math_log10  },
    { "exp",    math_exp    },
    { "abs",    math_abs    },
    { "sqrt",   math_sqrt   },
    { "pow",    math_pow    },
};

void cando_lib_math_register(CandoVM *vm)
{
    CandoValue math_val = cando_bridge_new_object(vm);
    CdoObject *math_obj = cando_bridge_resolve(vm, cando_as_handle(math_val));

    libutil_register_methods(vm, math_obj, math_methods,
                             CANDO_ARRAY_LEN(math_methods));

    set_const(math_obj, "pi",   3.14159265358979323846);
    set_const(math_obj, "tau",  6.28318530717958647692);
    set_const(math_obj, "e",    2.71828182845904523536);
    set_const(math_obj, "huge", HUGE_VAL);

    cando_vm_set_global(vm, "math", math_val, true);

    /* Phase 4.2: register f64->f64 fast paths so the JIT can compile
     * `math.sqrt(x)` and friends to a direct function-pointer call.
     * Order doesn't matter -- the registry looks up natives by their
     * slow C function pointer. */
    cando_vm_register_fast_native_f1(vm, math_sqrt,  sqrt);
    cando_vm_register_fast_native_f1(vm, math_abs,   fabs);
    cando_vm_register_fast_native_f1(vm, math_floor, floor);
    cando_vm_register_fast_native_f1(vm, math_ceil,  ceil);
    cando_vm_register_fast_native_f1(vm, math_log,   log);
    cando_vm_register_fast_native_f1(vm, math_log10, log10);
    cando_vm_register_fast_native_f1(vm, math_exp,   exp);
    cando_vm_register_fast_native_f1(vm, math_sinh,  sinh);
    cando_vm_register_fast_native_f1(vm, math_cosh,  cosh);
}
