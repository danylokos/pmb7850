#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "emu_dispatch.h"
#include "emu_runtime.h"

static const emu_engine_diagnostics_t all_diagnostics = {
    .engine_name = "cemu",
    .profile_name = "automatic",
    .capabilities = EMU_DIAG_MONITOR | EMU_DIAG_ARTIFACTS |
                    EMU_DIAG_DEBUGGER | EMU_DIAG_SNAPSHOT |
                    EMU_DIAG_COVERAGE | EMU_DIAG_RAW_LCD |
                    EMU_DIAG_SNAPSHOT_RESTORE,
    .trace_available = 1,
    .trace_mask = EMU_TRACE_MASK_ALL,
};

static int fail(FILE *err, const char *message, const char *detail) {
    fprintf(err, "error: ");
    fprintf(err, message, detail);
    fputc('\n', err);
    return 2;
}

static void top_level_usage(FILE *out, const char *program) {
    fprintf(out,
            "usage: %s <command> [options]\n\n"
            "Commands:\n"
            "  run    boot or resume a Siemens x55 handset\n\n"
            "Use '%s run --help' for CEMU runtime options.\n",
            program, program);
}

static int is_help(const char *argument) {
    return !strcmp(argument, "-h") || !strcmp(argument, "--help");
}

static int canonical_arguments(
        int argc, char **argv, char ***result, int *result_count,
        const char **engine, FILE *out, FILE *err) {
    if (argc == 2 && is_help(argv[1])) {
        top_level_usage(out, argv[0]);
        return 1;
    }
    if (argc < 2) {
        top_level_usage(err, argv[0]);
        return fail(err, "missing command: %s", "run");
    }
    if (strcmp(argv[1], "run")) {
        top_level_usage(err, argv[0]);
        return fail(err, "unknown command: %s", argv[1]);
    }
    if (argc == 3 && is_help(argv[2])) {
        emu_cli_usage(out, "emu run", &all_diagnostics);
        return 1;
    }

    char **forwarded = calloc((size_t)argc + 1u, sizeof *forwarded);
    if (!forwarded) return fail(err, "cannot allocate dispatcher arguments%s", "");
    int count = 1;
    int engine_seen = 0;
    *engine = "cemu";
    forwarded[0] = argv[0];
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--engine")) {
            if (engine_seen) {
                free(forwarded);
                return fail(err, "duplicate option: %s", "--engine");
            }
            if (++i >= argc) {
                free(forwarded);
                return fail(err, "missing value for %s", "--engine");
            }
            engine_seen = 1;
            if (strcmp(argv[i], "cemu") && strcmp(argv[i], "qemu")) {
                int rc = fail(err, "unavailable engine: %s", argv[i]);
                free(forwarded);
                return rc;
            }
            *engine = argv[i];
            continue;
        }
        uint64_t ignored = 0;
        int span = emu_cli_argument_span(
            argv[i], i + 1 < argc ? argv[i + 1] : NULL, &ignored, err);
        if (span < 0) {
            free(forwarded);
            return 2;
        }
        forwarded[count++] = argv[i];
        if (span == 2) forwarded[count++] = argv[++i];
    }
    forwarded[count] = NULL;
    *result = forwarded;
    *result_count = count;
    return 0;
}

int emu_dispatch(emu_dispatch_entry_t entry, int argc, char **argv,
                 const char *plain_runner,
                 const char *instrumented_runner,
                 emu_qemu_entry_fn qemu_entry, emu_exec_fn execute,
                 FILE *out, FILE *err) {
    if (argc < 1 || !argv || !argv[0] || !plain_runner ||
        !instrumented_runner || !execute || !out || !err)
        return 2;

    char **forwarded = argv;
    int forwarded_count = argc;
    const char *engine = "cemu";
    if (entry == EMU_ENTRY_CANONICAL) {
        int rc = canonical_arguments(
            argc, argv, &forwarded, &forwarded_count, &engine, out, err);
        if (rc) return rc == 1 ? 0 : rc;
        if (!strcmp(engine, "qemu")) {
            if (!qemu_entry) {
                free(forwarded);
                return fail(err, "unavailable engine: %s", engine);
            }
            rc = qemu_entry(forwarded_count, forwarded);
            free(forwarded);
            return rc;
        }
    }

    emu_runtime_profile_t profile =
        entry == EMU_ENTRY_LEGACY_INSTRUMENTED
        ? EMU_PROFILE_INSTRUMENTED : EMU_PROFILE_PLAIN;
    if (entry == EMU_ENTRY_CANONICAL) {
        uint64_t required = 0;
        if (emu_cli_required_diagnostics(
                forwarded_count, forwarded, &required, err) != 0) {
            free(forwarded);
            return 2;
        }
        if (required) profile = EMU_PROFILE_INSTRUMENTED;
    }

    const char *runner = profile == EMU_PROFILE_INSTRUMENTED
                       ? instrumented_runner : plain_runner;
    errno = 0;
    int rc = execute(runner, forwarded);
    int saved_errno = errno;
    if (entry == EMU_ENTRY_CANONICAL) free(forwarded);
    if (rc >= 0) return rc;
    errno = saved_errno;
    fprintf(err, "error: cannot execute %s: %s\n", runner, strerror(errno));
    return 2;
}
