/*
 * main.c -- CanDo script interpreter entry point.
 *
 * Thin wrapper around the libcando public API.  All initialization,
 * library loading, file reading, and execution are handled by the library;
 * main.c only handles command-line argument parsing and exit codes.
 *
 * Usage:
 *   cando <file.cdo>            Execute a script
 *   cando <file.cdo> --disasm   Disassemble then execute
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
        fprintf(stderr, "usage: %s <file.cdo> [--disasm]\n", argv[0]);
        return 1;
    }

    const char *path   = argv[1];
    bool        disasm = (argc >= 3 && strcmp(argv[2], "--disasm") == 0);

    /* Create a new VM and load all standard libraries. */
    CandoVM *vm = cando_open();
    if (!vm) {
        fprintf(stderr, "cando: failed to create VM (out of memory?)\n");
        return 1;
    }
    cando_openlibs(vm);

    int rc = 0;

    if (disasm) {
        /* Load file, disassemble, then execute. */
        CandoChunk *chunk = NULL;
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "cando: cannot open '%s'\n", path);
            cando_close(vm);
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
    return rc;
}
