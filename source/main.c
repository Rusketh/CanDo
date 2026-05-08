/*
 * main.c -- CanDo script interpreter entry point.
 *
 * Thin wrapper around the libcando public API.  All initialization,
 * library loading, file reading, and execution are handled by the library;
 * main.c only handles command-line argument parsing and exit codes.
 *
 * Usage:
 *   cando <file.cdo> [interpreter-flags] [args...]
 *
 * Interpreter flags (consumed here, not forwarded to the script):
 *   --disasm        disassemble the chunk before execution
 *   --jit           request the JIT (no-op until it lands; see
 *                   docs/jit-plan.md)
 *   --no-jit        force-disable the JIT (no-op until it lands)
 *   --jit-stats     print one-line JIT summary at exit (no-op until
 *                   the JIT lands)
 *
 * Anything else after the script path is forwarded to the script via
 * the global `args` array.
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
        fprintf(stderr, "usage: %s <file.cdo> "
                        "[--disasm] [--jit|--no-jit] [--jit-stats] "
                        "[args...]\n", argv[0]);
        return 1;
    }

    const char *path        = argv[1];
    bool        disasm      = false;
    bool        jit_stats   = false;
    /* The --jit / --no-jit flags are parsed and accepted today so scripts
     * and CI invocations can rely on the spelling, but they have no effect
     * on the interpreter -- the JIT is not yet implemented.  See
     * docs/jit-plan.md for the roadmap.  Keep the variables to silence
     * unused-warning linting when the JIT lands. */
    bool        jit_request = false;
    bool        jit_disable = false;
    (void)jit_request;
    (void)jit_disable;

    /* Collect script args: everything after argv[1] that isn't an
     * interpreter flag. */
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
            } else if (strcmp(argv[i], "--jit") == 0) {
                jit_request = true;
            } else if (strcmp(argv[i], "--no-jit") == 0) {
                jit_disable = true;
            } else if (strcmp(argv[i], "--jit-stats") == 0) {
                jit_stats = true;
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

    if (jit_stats) {
        /* Stub: the JIT does not yet exist.  Once it lands this will
         * print traces compiled / aborts / side-exits / mcode bytes. */
        fprintf(stderr, "jit: not built (see docs/jit-plan.md)\n");
    }

    return rc;
}
