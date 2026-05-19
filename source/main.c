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
 *   --jit           enable JIT profiling counters + recorder (no
 *                   machine code yet -- traces are constructed in
 *                   IR and stored for inspection; see Phase 4+ of
 *                   docs/jit-plan.md for codegen).
 *   --no-jit        force-disable the JIT.  Wins over --jit,
 *                   --jit-stats, --jit-dump, and CANDO_JIT.
 *   --jit-stats     print a one-line summary of profiling counters at
 *                   exit.  Always implies --jit (otherwise the output
 *                   is meaningless); use --no-jit if you want to
 *                   suppress the print without disabling globally.
 *   --jit-dump      after stats, print the IR of every compiled
 *                   trace.  Implies --jit.
 *   --no-console    drop the inherited console window at startup
 *                   (Windows: FreeConsole(); POSIX: dup /dev/null
 *                   over fd 0/1/2) and disable the console standard
 *                   library for the lifetime of the VM.  Useful when
 *                   launching CanDo scripts from a GUI shortcut.
 *
 * Environment variables:
 *   CANDO_JIT=1     equivalent to --jit when no CLI flag overrides.
 *                   Any other value (including 0 or empty) is ignored.
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
#include "vm/vm.h"

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "CanDo %s\n", CANDO_VERSION);
        fprintf(stderr, "usage: %s <file.cdo> "
                        "[--disasm] [--jit|--no-jit] [--jit-stats] "
                        "[--no-console] [args...]\n", argv[0]);
        return 1;
    }

    const char *path        = NULL;
    bool        disasm      = false;
    bool        jit_stats   = false;
    bool        jit_request = false;
    bool        jit_disable = false;
    bool        jit_dump    = false;
    bool        no_console  = false;

    /* Walk argv: interpreter flags can appear before or after the
     * script path; the first non-flag arg is the script.  Everything
     * after the script that isn't an interpreter flag becomes a
     * script-arg. */
    const char **script_argv = NULL;
    int          script_argc = 0;
    if (argc > 1) {
        script_argv = (const char **)malloc(sizeof(char *) * (size_t)(argc - 1));
        if (!script_argv) {
            fprintf(stderr, "cando: out of memory\n");
            return 1;
        }
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--disasm") == 0) {
                disasm = true;
            } else if (strcmp(argv[i], "--jit") == 0) {
                jit_request = true;
            } else if (strcmp(argv[i], "--no-jit") == 0) {
                jit_disable = true;
            } else if (strcmp(argv[i], "--jit-stats") == 0) {
                jit_stats = true;
            } else if (strcmp(argv[i], "--jit-dump") == 0) {
                jit_dump = true;
            } else if (strcmp(argv[i], "--no-console") == 0) {
                no_console = true;
            } else if (!path) {
                path = argv[i];
            } else {
                script_argv[script_argc++] = argv[i];
            }
        }
    }

    if (!path) {
        fprintf(stderr, "cando: missing script path\n");
        free(script_argv);
        return 1;
    }

    /* --no-console drops the inherited console window before the VM
     * is even spun up; the library is then disabled for the script's
     * lifetime so any console.* call throws cleanly.  Done before
     * cando_open so a FreeConsole-mid-startup race can't drop a log
     * line written by the VM init. */
    if (no_console) {
        cando_console_detach();
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

    if (no_console) {
        cando_console_set_enabled(vm, false);
    }

    /* JIT profiling state.  Resolution order:
     *   1. --no-jit on the CLI wins over everything (force off).
     *   2. --jit / --jit-stats on the CLI enable counters.
     *   3. CANDO_JIT=1 in the environment enables counters.
     *   4. Default: off. */
    if (jit_disable) {
        cando_jit_disable(vm);
    } else if (jit_request || jit_stats || jit_dump) {
        cando_jit_enable(vm);
    } else {
        const char *env = getenv("CANDO_JIT");
        if (env && env[0] && env[0] != '0')
            cando_jit_enable(vm);
    }

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

    if (jit_stats) {
        /* Always prints, even with --no-jit -- in that case the counters
         * are zero by construction, which is itself useful information
         * (e.g. "did this run actually trigger anything?"). */
        CandoJitStats st = cando_jit_get_stats(vm);
        fprintf(stderr,
            "jit: backedges=%llu func_entries=%llu iter_next=%llu "
            "trace_starts=%u traces_compiled=%u trace_aborts=%u "
            "trace_iters=%llu trace_exits=%u "
            "hot_pcs=%u blacklisted=%u traces_evicted=%u",
            (unsigned long long)st.backedge_hits,
            (unsigned long long)st.func_entry_hits,
            (unsigned long long)st.iter_next_hits,
            st.trace_starts,
            st.traces_compiled,
            st.trace_aborts,
            (unsigned long long)st.trace_iters,
            st.trace_exits,
            st.hot_pcs,
            st.blacklisted_pcs,
            st.traces_evicted);
        if (st.trace_aborts > 0 && cando_jit_is_enabled(vm)) {
            const char *reason = cando_jit_last_abort(vm);
            if (reason && reason[0])
                fprintf(stderr, " last_abort=\"%s\"", reason);
        }
        fputc('\n', stderr);
    }

    if (jit_dump) {
        cando_jit_dump_traces(vm, stderr);
    }

    cando_close(vm);
    free(script_argv);

    return rc;
}
