/*
 * main.c -- CanDo script interpreter entry point.
 *
 * Thin wrapper around the libcando public API.  All initialization,
 * library loading, file reading, and execution are handled by the library;
 * main.c only handles command-line argument parsing and exit codes.
 *
 * Usage:
 *   cando <file.cdo> [--disasm] [args...]
 *
 * Anything after the script path that is not the `--disasm` switch is
 * forwarded to the script via the global `args` array.
 *
 * Must compile with gcc -std=c11.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cando.h"

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "CanDo %s\n", CANDO_VERSION);
        fprintf(stderr, "usage: %s <file.cdo> [--disasm] [args...]\n", argv[0]);
        return 1;
    }

    const char *path   = argv[1];
    bool        disasm = false;

    /* Collect script args: everything after argv[1] except the --disasm
     * switch (which is consumed by the interpreter, not the script). */
    const char **script_argv = NULL;
    int          script_argc = 0;
    if (argc > 2) {
        script_argv = (const char **)malloc(sizeof(char *) * (size_t)(argc - 2));
        if (!script_argv) {
            fprintf(stderr, "cando: out of memory\n");
            return 1;
        }
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--disasm") == 0) {
                disasm = true;
            } else {
                script_argv[script_argc++] = argv[i];
            }
        }
    }

    /* Create a new VM and load all standard libraries. */
    CandoVM *vm = cando_open();
    if (!vm) {
        fprintf(stderr, "cando: failed to create VM (out of memory?)\n");
        free(script_argv);
        return 1;
    }
    cando_openlibs(vm);
    cando_set_args(vm, script_argc, script_argv);

    int rc = 0;

    if (disasm) {
        /* Load file, disassemble, then execute. */
        CandoChunk *chunk = NULL;
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "cando: cannot open '%s'\n", path);
            cando_close(vm);
            free(script_argv);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        rewind(f);
        if (fsize > 0) {
            char *src = (char *)malloc((size_t)(fsize + 1));
            size_t nr  = fread(src, 1, (size_t)fsize, f);
            src[nr]    = '\0';
            fclose(f);
            int lr = cando_loadstring(vm, src, path, &chunk);
            free(src);
            if (lr != CANDO_OK || !chunk) {
                fprintf(stderr, "cando: %s\n", cando_errmsg(vm));
                cando_close(vm);
                free(script_argv);
                return 1;
            }
            cando_chunk_disasm(chunk, stderr);
            CandoVMResult result = cando_vm_exec(vm, chunk);
            if (result == VM_RUNTIME_ERR) {
                fprintf(stderr, "cando: %s\n", cando_errmsg(vm));
                rc = 1;
            }
            cando_chunk_free(chunk);
        } else {
            fclose(f);
        }
    } else {
        /* Normal execution. */
        int exec_rc = cando_dofile(vm, path);
        if (exec_rc != CANDO_OK) {
            fprintf(stderr, "cando: %s\n", cando_errmsg(vm));
            rc = 1;
        }
    }

    cando_close(vm);
    free(script_argv);
    return rc;
}
