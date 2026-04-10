/*
 * main.c -- Cando script interpreter entry point.
 *
 * Reads a .cdo file, compiles it with the Cando parser, and executes the
 * resulting bytecode via the Cando VM (cando_vm_exec).
 *
 * Must compile with gcc -std=c11.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/common.h"
#include "core/value.h"
#include "vm/vm.h"
#include "parser/parser.h"
#include "object/object.h"

#include "vm/debug.h"
#include "natives.h"
#include "lib/math.h"
#include "lib/file.h"
#include "lib/eval.h"
#include "lib/string.h"
#include "lib/include.h"
#include "lib/json.h"
#include "lib/csv.h"
#include "lib/thread.h"
#include "lib/os.h"
#include "lib/datetime.h"
#include "lib/array.h"
#include "lib/object.h"
#include "lib/crypto.h"
#include "lib/process.h"
#include "lib/net.h"

#include <limits.h>

#if defined(_WIN32) || defined(_WIN64)
#  include <direct.h>
static char *realpath(const char *path, char *out) {
    return _fullpath(out, path, PATH_MAX);
}
#endif

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.cdo>\n", argv[0]);
        return 1;
    }

    /* Read source file */
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", argv[1]);
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fprintf(stderr, "error: cannot seek '%s'\n", argv[1]);
        return 1;
    }
    long fsize = ftell(f);
    rewind(f);
    if (fsize < 0) {
        fclose(f);
        fprintf(stderr, "error: cannot determine size of '%s'\n", argv[1]);
        return 1;
    }

    char *source = (char *)cando_alloc((usize)(fsize + 1));
    usize nread  = fread(source, 1, (usize)fsize, f);
    source[nread] = '\0';
    fclose(f);

    /* Resolve the script's absolute path so relative include() calls work. */
    char script_path[PATH_MAX];
    if (!realpath(argv[1], script_path)) {
        /* Fall back to the argument as-is if realpath fails (e.g. stdin). */
        strncpy(script_path, argv[1], PATH_MAX - 1);
        script_path[PATH_MAX - 1] = '\0';
    }

    /* Parse into a VM chunk */
    CandoChunk *chunk = cando_chunk_new(script_path, 0, false);
    CandoParser parser;
    cando_parser_init(&parser, source, nread, chunk);

    bool ok = cando_parse(&parser);
    cando_free(source);

    if (!ok) {
        fprintf(stderr, "parse error: %s\n", cando_parser_error(&parser));
        cando_chunk_free(chunk);
        return 1;
    }

    /* Initialize object layer (intern table + meta-keys) */
    cdo_object_init();

    /* Initialize VM and register native functions */
    //TODO: This should be its own function, for use with eval :D
    CandoVM vm;
    cando_vm_init(&vm, NULL);

    for (u32 i = 0; cando_native_names[i] && cando_native_table[i]; i++) {
        cando_vm_register_native(&vm, cando_native_names[i], cando_native_table[i]);
    }

    cando_lib_math_register(&vm);
    cando_lib_file_register(&vm);
    cando_lib_eval_register(&vm);
    cando_lib_string_register(&vm);
    cando_lib_include_register(&vm);
    cando_lib_json_register(&vm);
    cando_lib_csv_register(&vm);
    cando_lib_thread_register(&vm);
    cando_lib_os_register(&vm);
    cando_lib_datetime_register(&vm);
    cando_lib_array_register(&vm);
    cando_lib_object_register(&vm);
    cando_lib_crypto_register(&vm);
    cando_lib_process_register(&vm);
    cando_lib_net_register(&vm);

    /* Execute */
    if (argc >= 3 && strcmp(argv[2], "--disasm") == 0)
        cando_chunk_disasm(chunk, stderr);
    CandoVMResult result = cando_vm_exec(&vm, chunk);
    if (result == VM_RUNTIME_ERR) {
        fprintf(stderr, "runtime error: %s\n", vm.error_msg);
    }

    cando_vm_destroy(&vm);
    cdo_object_destroy_globals();
    cando_chunk_free(chunk);
    return (result == VM_OK || result == VM_HALT) ? 0 : 1;
}

