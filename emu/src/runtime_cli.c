#include <errno.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "emu_runtime.h"

#define CEMU_INSTRUMENTED 1
#define CEMU_TRACE_PARQUET 1

typedef enum {
    OPT_FLAG,
    OPT_STRING,
    OPT_COUNT,
    OPT_SPECIAL
} option_type_t;

typedef enum {
    O_HELP,
    O_DEVICE,
    O_FSN,
    O_IMEI,
    O_SIM,
    O_BATTERY_LEVEL,
    O_BATTERY_CHARGING,
    O_SYNTH,
    O_PATCH,
    O_LIMIT,
    O_UI_SOCKET,
    O_SERIAL_PTY,
    O_QEMU_BINARY,
    O_GDB,
    O_GDB_BINARY,
    O_BENCHMARK_JSON,
    O_FROM_SNAPSHOT,
    O_EEPROM_OVERLAY,
    O_BATCH_IDLE,
#if CEMU_INSTRUMENTED
    O_LABEL,
    O_TRACE,
    O_DRCOV,
    O_LCD_FRAMES,
    O_LCD_DDRAM_FRAMES,
    O_TRACE_FROM_ICOUNT,
    O_TRACE_FROM_PC,
    O_SNAPSHOT,
    O_SNAPSHOT_FULL_BINS,
    O_DUMP_FLASH,
    O_SNAPSHOT_AT,
    O_SUMMARY,
    O_SHOW_WRITES,
    O_MONITOR,
    O_MONITOR_LOOP_THRESHOLD,
    O_MONITOR_STALL_WINDOW,
    O_COMMAND,
    O_SCRIPT,
    O_INTERACTIVE,
#endif
} option_id_t;

typedef struct {
    const char *short_name;
    const char *long_name;
    option_type_t type;
    option_id_t id;
    const char *value_name;
    const char *help;
    const char *group;
    uint64_t required;
} option_desc_t;

static int fail(FILE *, const char *, const char *);

#define OPT(s, l, t, id, value, text, group) \
    { s, l, t, id, value, text, group, 0 }
#define DIAG(s, l, t, id, value, text, group, cap) \
    { s, l, t, id, value, text, group, cap }
static const option_desc_t OPTIONS[] = {
    OPT("-h", "--help", OPT_FLAG, O_HELP, NULL, "show this help and exit", "General"),
    OPT(NULL, "--device", OPT_STRING, O_DEVICE, "NAME",
        "device config (c55/a52/a55/a60/a62/a65/c60/cf62/m55/mc60/s55/sl55); default: auto-detect", "Input"),
    OPT(NULL, "--fsn", OPT_STRING, O_FSN, "HEX",
        "handset FSN (8 hex digits)", "Input"),
    OPT(NULL, "--imei", OPT_STRING, O_IMEI, "DIGITS",
        "AM29 SecSi IMEI mirror (14 digits; requires --fsn)", "Input"),
    OPT(NULL, "--sim", OPT_FLAG, O_SIM, NULL,
        "attach the deterministic SIM-card profile", "Input"),
    OPT(NULL, "--battery-level", OPT_COUNT, O_BATTERY_LEVEL, "PERCENT",
        "battery level from 0 to 100 (default: 100)", "Input"),
    OPT(NULL, "--battery-charging", OPT_STRING, O_BATTERY_CHARGING, "on|off",
        "charger connection state (default: off)", "Input"),
    OPT(NULL, "--synth", OPT_SPECIAL, O_SYNTH, "NAMES",
        "toggle synthetic behaviors; use 'list' to show names", "Input"),
    OPT(NULL, "--patch", OPT_SPECIAL, O_PATCH, "NAMES",
        "apply startup firmware patches; use 'list' for the catalog", "Input"),
    DIAG(NULL, "--from-snapshot", OPT_STRING, O_FROM_SNAPSHOT, "DIR",
        "resume state from a snapshot directory", "Input",
        EMU_DIAG_SNAPSHOT_RESTORE),
    OPT(NULL, "--eeprom-overlay", OPT_STRING, O_EEPROM_OVERLAY, "BUNDLE.json",
        "seed existing EEPROM identity records from a generated bundle", "Input"),
    OPT(NULL, "--limit", OPT_COUNT, O_LIMIT, "N",
        "stop after N ticks (accepts k/m/0x; default 8m)", "Run control"),
    OPT(NULL, "--ui-socket", OPT_STRING, O_UI_SOCKET, "PATH",
        "serve the live LCD/keypad/run-control protocol", "Run control"),
    OPT(NULL, "--serial-pty", OPT_STRING, O_SERIAL_PTY, "PATH",
        "expose ASC0 through a newly allocated PTY at PATH", "Run control"),
    OPT(NULL, "--qemu-binary", OPT_STRING, O_QEMU_BINARY, "PATH",
        "use this qemu-system-c166 for --engine qemu", "Run control"),
    OPT(NULL, "--gdb", OPT_STRING, O_GDB, "PORT",
        "expose QEMU's GDB stub on 127.0.0.1:PORT and wait at reset", "Run control"),
    OPT(NULL, "--benchmark-json", OPT_FLAG, O_BENCHMARK_JSON, NULL,
        "print one machine-readable run-statistics object", "Run control"),
    OPT(NULL, "--batch-idle", OPT_FLAG, O_BATCH_IDLE, NULL,
        "batch proven event-free CPU IDLE ticks", "Run control"),
#if CEMU_INSTRUMENTED
    DIAG(NULL, "--monitor", OPT_FLAG, O_MONITOR, NULL,
        "collect progress metrics and stop on a monitor verdict", "Monitoring", EMU_DIAG_MONITOR),
    DIAG(NULL, "--monitor-loop-threshold", OPT_COUNT, O_MONITOR_LOOP_THRESHOLD, "N",
        "iterations before a hot loop is judged (default 256)", "Monitoring", EMU_DIAG_MONITOR),
    DIAG(NULL, "--monitor-stall-window", OPT_COUNT, O_MONITOR_STALL_WINDOW, "N",
        "global stall watchdog window (default 2m)", "Monitoring", EMU_DIAG_MONITOR),
    DIAG(NULL, "--summary", OPT_FLAG, O_SUMMARY, NULL,
        "print the full run summary; implies --monitor", "Monitoring", EMU_DIAG_MONITOR),
    DIAG(NULL, "--show-writes", OPT_FLAG, O_SHOW_WRITES, NULL,
        "include write counts and write-only cells in --summary", "Monitoring", EMU_DIAG_MONITOR),
    DIAG(NULL, "--label", OPT_STRING, O_LABEL, "NAME",
        "name the shots/ output directory", "Artifacts", EMU_DIAG_ARTIFACTS),
    DIAG(NULL, "--trace", OPT_SPECIAL, O_TRACE, "[SELECTORS]",
        "write trace/trace.parquet; bare means all, 'list' shows selectors", "Artifacts", EMU_DIAG_ARTIFACTS),
    DIAG(NULL, "--drcov", OPT_FLAG, O_DRCOV, NULL,
        "write coverage/cov.drcov", "Artifacts", EMU_DIAG_COVERAGE),
    DIAG(NULL, "--lcd-frames", OPT_FLAG, O_LCD_FRAMES, NULL,
        "write strict LCD PNG frames and an animated GIF", "Artifacts", EMU_DIAG_ARTIFACTS),
    DIAG(NULL, "--lcd-ddram-frames", OPT_FLAG, O_LCD_DDRAM_FRAMES, NULL,
        "write raw controller-DDRAM PNG frames", "Artifacts", EMU_DIAG_RAW_LCD),
    DIAG(NULL, "--trace-from-icount", OPT_COUNT, O_TRACE_FROM_ICOUNT, "N",
        "arm tracing at icount N (requires --trace)", "Artifacts", EMU_DIAG_ARTIFACTS),
    DIAG(NULL, "--trace-from-pc", OPT_STRING, O_TRACE_FROM_PC, "ADDR",
        "arm tracing when PC first reaches ADDR (requires --trace)", "Artifacts", EMU_DIAG_ARTIFACTS),
    DIAG(NULL, "--snapshot", OPT_FLAG, O_SNAPSHOT, NULL,
        "write an engine-native snapshot directory on stop", "Artifacts", EMU_DIAG_SNAPSHOT),
    DIAG(NULL, "--snapshot-full-bins", OPT_FLAG, O_SNAPSHOT_FULL_BINS, NULL,
        "also write CPU-visible fixed-boundary bins; implies --snapshot", "Artifacts", EMU_DIAG_SNAPSHOT),
    DIAG(NULL, "--dump-flash", OPT_FLAG, O_DUMP_FLASH, NULL,
        "write the physical main flash array when the run stops", "Artifacts", EMU_DIAG_ARTIFACTS),
    DIAG(NULL, "--snapshot-at", OPT_COUNT, O_SNAPSHOT_AT, "N",
        "capture at icount N (requires --snapshot)", "Artifacts", EMU_DIAG_SNAPSHOT),
    DIAG("-c", "--command", OPT_STRING, O_COMMAND, "CMDS",
        "run a debugger command (QEMU: native GDB; repeatable)", "Debugger", EMU_DIAG_DEBUGGER | EMU_DIAG_MANAGED_GDB),
    DIAG(NULL, "--script", OPT_STRING, O_SCRIPT, "FILE",
        "run a debugger command file", "Debugger", EMU_DIAG_DEBUGGER | EMU_DIAG_MANAGED_GDB),
    DIAG("-i", "--interactive", OPT_FLAG, O_INTERACTIVE, NULL,
        "drop into the debugger REPL", "Debugger", EMU_DIAG_DEBUGGER | EMU_DIAG_MANAGED_GDB),
    DIAG(NULL, "--gdb-binary", OPT_STRING, O_GDB_BINARY, "PATH",
        "launch this C166 GDB (QEMU managed debugging)", "Debugger", EMU_DIAG_MANAGED_GDB),
#endif
};
#undef OPT
#undef DIAG

static int parse_count(const char *s, uint64_t *out) {
    if (!s || !*s || s[0] == '-') return -1;
    size_t len = strlen(s);
    uint64_t multiplier = 1;
    if (s[len - 1] == 'k' || s[len - 1] == 'K') multiplier = 1000;
    else if (s[len - 1] == 'm' || s[len - 1] == 'M') multiplier = 1000000;

    char buf[64];
    if (len >= sizeof buf) return -1;
    memcpy(buf, s, len + 1);
    if (multiplier != 1) {
        if (len == 1) return -1;
        buf[len - 1] = '\0';
    }

    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(buf, &end, 0);
    if (errno || !end || *end || value > UINT64_MAX / multiplier) return -1;
    *out = (uint64_t)value * multiplier;
    return 0;
}

static int parse_fsn(const char *text, uint32_t *out) {
    if (!text || !out) return -1;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    if (strlen(text) != 8u) return -1;
    uint32_t value = 0;
    for (unsigned i = 0; i < 8; i++) {
        int nibble = text[i] >= '0' && text[i] <= '9' ? text[i] - '0' :
                     text[i] >= 'a' && text[i] <= 'f' ? text[i] - 'a' + 10 :
                     text[i] >= 'A' && text[i] <= 'F' ? text[i] - 'A' + 10 : -1;
        if (nibble < 0) return -1;
        value = (value << 4) | (uint32_t)nibble;
    }
    *out = value;
    return 0;
}

static int parse_port(const char *text, unsigned *out) {
    if (!text || !*text) return -1;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p < '0' || *p > '9') return -1;
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !end || *end || value == 0 || value > 65535ul) return -1;
    *out = (unsigned)value;
    return 0;
}

static int parse_address(const char *s, long *out) {
    if (!s || !*s || s[0] == '-') return -1;
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(s, &end, 0);
    if (errno || !end || *end || value > 0xFFFFFFul) return -1;
    *out = (long)value;
    return 0;
}

static const option_desc_t *find_option(
        const char *arg, const char **inline_value,
        const emu_engine_diagnostics_t *diagnostics) {
    *inline_value = NULL;
    for (size_t i = 0; i < sizeof OPTIONS / sizeof OPTIONS[0]; i++) {
        const option_desc_t *d = &OPTIONS[i];
        if (d->required &&
            (!diagnostics || !(diagnostics->capabilities & d->required)))
            continue;
        if ((d->short_name && !strcmp(arg, d->short_name)) ||
            !strcmp(arg, d->long_name))
            return d;
        size_t n = strlen(d->long_name);
#if CEMU_INSTRUMENTED
        if (d->id == O_TRACE && !strncmp(arg, d->long_name, n) && arg[n] == '=') {
            *inline_value = arg + n + 1;
            return d;
        }
#else
        (void)n;
#endif
    }
    return NULL;
}

static const option_desc_t *find_any_option(
        const char *argument, const char **inline_value) {
    return find_option(argument, inline_value, &
        (const emu_engine_diagnostics_t){
            .capabilities = UINT64_MAX,
        });
}

int emu_cli_argument_span(const char *argument, const char *next,
                          uint64_t *required, FILE *err) {
    if (!argument || !required || !err) return -1;
    if (argument[0] != '-') return 1;
    const char *inline_value = NULL;
    const option_desc_t *option = find_any_option(argument, &inline_value);
    if (!option) return fail(err, "unknown argument: %s", argument);
    *required |= option->required;
    int needs_value = option->type == OPT_STRING ||
                      option->type == OPT_COUNT ||
                      option->type == OPT_SPECIAL;
    if (option->id == O_TRACE) needs_value = 0;
    if (needs_value && !inline_value) {
        if (!next) return fail(err, "missing value for %s", option->long_name);
        return 2;
    }
    if (option->id == O_TRACE && !inline_value && next) {
        char bad[64];
        emu_trace_mask_t mask;
        if (!strcmp(next, "list") || !strcmp(next, "help") ||
            emu_trace_parse_selectors(next, &mask, bad, sizeof bad) == 0)
            return 2;
    }
    return 1;
}

int emu_cli_required_diagnostics(
        int argc, char **argv, uint64_t *required, FILE *err) {
    if (argc < 1 || !argv || !required || !err) return -1;
    *required = 0;
    for (int i = 1; i < argc; i++) {
        int span = emu_cli_argument_span(
            argv[i], i + 1 < argc ? argv[i + 1] : NULL, required, err);
        if (span < 0) return -1;
        i += span - 1;
    }
    return 0;
}

void emu_cli_options_init(
        emu_cli_options_t *o, const emu_engine_diagnostics_t *diagnostics) {
    memset(o, 0, sizeof *o);
    o->limit = 8000000;
    o->battery_level = 100;
    o->synth_mask = diagnostics ? diagnostics->synthetic_defaults : 0;
#if CEMU_INSTRUMENTED
    o->trace_mask = EMU_TRACE_MASK_ALL;
    o->trace_from_pc = -1;
    o->monitor_loop_threshold = 256;
    o->monitor_stall_window = 2000000;
#endif
}

static int fail(FILE *err, const char *message, const char *arg) {
    fprintf(err, "error: ");
    fprintf(err, message, arg);
    fputc('\n', err);
    return -1;
}

static int trace_spec_requests_gsm_l1(const char *spec) {
    const char *p = spec;
    while (p && *p) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && isspace((unsigned char)end[-1])) end--;
        size_t n = (size_t)(end - start);
        if ((n == 3 && !strncmp(start, "gsm", n)) ||
            (n == 15 && !strncmp(start, "gsm_l1_firmware", n)))
            return 1;
    }
    return 0;
}

int emu_cli_parse(
        emu_cli_options_t *o, int argc, char **argv, FILE *err,
        const emu_engine_diagnostics_t *diagnostics) {
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] != '-') {
            if (o->flash_path)
                return fail(err, "multiple flash images: %s", arg);
            o->flash_path = arg;
            continue;
        }

        const char *inline_value;
        const option_desc_t *d = find_option(arg, &inline_value, diagnostics);
        if (!d) return fail(err, "unknown argument: %s", arg);

        const char *value = inline_value;
        int requires_value = d->type == OPT_STRING || d->type == OPT_COUNT ||
                             d->type == OPT_SPECIAL;
#if CEMU_INSTRUMENTED
        if (d->id == O_TRACE) requires_value = 0;
#endif
        if (requires_value) {
            if (inline_value)
                return fail(err, "option does not accept '=' form: %s", arg);
            if (++i >= argc)
                return fail(err, "missing value for %s", d->long_name);
            value = argv[i];
        }

        uint64_t count = 0;
        if (d->type == OPT_COUNT && parse_count(value, &count) != 0)
            return fail(err, "invalid numeric value for %s", d->long_name);

        switch (d->id) {
            case O_GDB_BINARY: o->gdb_binary = value; break;
            case O_HELP: o->help = 1; break;
            case O_DEVICE: o->device_name = value; break;
            case O_FROM_SNAPSHOT: o->from_snapshot = value; break;
            case O_EEPROM_OVERLAY: o->eeprom_overlay_path = value; break;
            case O_UI_SOCKET: o->ui_socket_path = value; break;
            case O_SERIAL_PTY: o->serial_pty_path = value; break;
            case O_QEMU_BINARY: o->qemu_binary = value; break;
            case O_GDB:
                if (parse_port(value, &o->gdb_port) != 0)
                    return fail(err, "GDB port must be 1..65535: %s", value);
                o->gdb_enabled = 1;
                break;
            case O_LIMIT: o->limit = count; o->limit_explicit = 1; break;
            case O_BENCHMARK_JSON: o->benchmark_json = 1; break;
            case O_BATCH_IDLE: o->batch_idle = 1; break;
            case O_FSN:
                if (parse_fsn(value, &o->fsn) != 0)
                    return fail(err, "invalid FSN: %s", value);
                o->fsn_hex = value;
                o->fsn_set = 1;
                break;
            case O_IMEI:
                if (strlen(value) != 14u || strspn(value, "0123456789") != 14u)
                    return fail(err, "invalid IMEI (expected 14 decimal digits): %s", value);
                o->imei = value;
                break;
            case O_SIM:
                o->sim_stub = 1;
                if (i + 1 < argc && (!strcmp(argv[i + 1], "none") ||
                                     !strcmp(argv[i + 1], "test")))
                    return fail(err, "--sim is now a flag; remove legacy value: %s",
                                argv[i + 1]);
                break;
            case O_BATTERY_LEVEL:
                if (count > 100)
                    return fail(err, "battery level must be 0..100: %s", value);
                o->battery_level = (unsigned)count;
                o->battery_level_set = 1;
                break;
            case O_BATTERY_CHARGING:
                if (!strcmp(value, "on")) o->battery_charging = 1;
                else if (!strcmp(value, "off")) o->battery_charging = 0;
                else
                    return fail(err, "battery charging must be on or off: %s", value);
                o->battery_charging_set = 1;
                break;
            case O_SYNTH: {
                if (!strcmp(value, "list") || !strcmp(value, "help")) {
                    o->synth_spec = value;
                    break;
                }
                const char *bad = NULL;
                if (!diagnostics || !diagnostics->synthetic_parse ||
                    diagnostics->synthetic_parse(&o->synth_mask, value, &bad) != 0)
                    return fail(err, "unknown synth behavior: %s", value);
                break;
            }
            case O_PATCH: {
                if (!strcmp(value, "list") || !strcmp(value, "help")) {
                    o->patch_list = 1;
                    break;
                }
                char bad[96];
                if (emu_patch_parse(&o->firmware_patches, value,
                                    bad, sizeof bad) != 0)
                    return fail(err, "unknown firmware patch: %s",
                                bad[0] ? bad : value);
                break;
            }
#if CEMU_INSTRUMENTED
            case O_LABEL: o->label = value; break;
            case O_TRACE: {
                if (!value && i + 1 < argc &&
                    (!strcmp(argv[i + 1], "list") ||
                     !strcmp(argv[i + 1], "help")))
                    value = argv[++i];
                if (value && (!strcmp(value, "list") ||
                              !strcmp(value, "help"))) {
                    o->trace_list = 1;
                    break;
                }
                o->want_trace = 1;
                o->trace_mask = EMU_TRACE_MASK_ALL;
                if (!value && i + 1 < argc) {
                    char bad[64];
                    emu_trace_mask_t parsed;
                    if (emu_trace_parse_selectors(argv[i + 1], &parsed, bad, sizeof bad) == 0)
                        value = argv[++i];
                }
                if (value) {
                    char bad[64];
                    if (emu_trace_parse_selectors(value, &o->trace_mask, bad, sizeof bad) != 0)
                        return fail(err, "unknown trace selector: %s", bad[0] ? bad : value);
                    o->gsm_l1_trace_explicit =
                        trace_spec_requests_gsm_l1(value);
                }
                break;
            }
            case O_DRCOV: o->want_drcov = 1; break;
            case O_LCD_FRAMES: o->want_lcd_frames = 1; break;
            case O_LCD_DDRAM_FRAMES: o->want_lcd_ddram_frames = 1; break;
            case O_TRACE_FROM_ICOUNT:
                o->trace_from_icount = count; o->trace_from_icount_set = 1; break;
            case O_TRACE_FROM_PC:
                if (parse_address(value, &o->trace_from_pc) != 0)
                    return fail(err, "invalid address for --trace-from-pc: %s", value);
                break;
            case O_SNAPSHOT: o->want_snapshot = 1; break;
            case O_SNAPSHOT_FULL_BINS:
                o->want_snapshot = 1; o->want_snapshot_full_bins = 1; break;
            case O_DUMP_FLASH: o->want_dump_flash = 1; break;
            case O_SNAPSHOT_AT:
                o->snapshot_at = count; o->snapshot_at_set = 1; break;
            case O_SUMMARY: o->summary = 1; o->monitor = 1; break;
            case O_SHOW_WRITES: o->show_writes = 1; break;
            case O_MONITOR: o->monitor = 1; break;
            case O_MONITOR_LOOP_THRESHOLD:
                o->monitor_loop_threshold = count; o->monitor_loop_threshold_set = 1; break;
            case O_MONITOR_STALL_WINDOW:
                o->monitor_stall_window = count; o->monitor_stall_window_set = 1; break;
            case O_COMMAND:
            case O_SCRIPT:
                if (diagnostics && (diagnostics->capabilities & EMU_DIAG_MANAGED_GDB)) {
                    if (o->gdb_action_count == EMU_GDB_MAX_ACTIONS)
                        return fail(err, "too many GDB startup actions%s", "");
                    o->gdb_actions[o->gdb_action_count++] = (emu_gdb_action_t){
                        .script = d->id == O_SCRIPT, .value = value,
                    };
                }
                if (d->id == O_COMMAND) o->dbg_command = value;
                else o->dbg_script = value;
                break;
            case O_INTERACTIVE: o->dbg_interactive = 1; break;
#endif
        }
    }

#if CEMU_INSTRUMENTED
    int debug_mode = o->dbg_command || o->dbg_script || o->dbg_interactive;
    if (o->want_trace && (!diagnostics || !diagnostics->trace_available))
        return fail(err, "--trace unavailable: rebuild with CEMU_TRACE_PARQUET=1", NULL);
    if ((o->trace_from_icount_set || o->trace_from_pc >= 0) && !o->want_trace)
        return fail(err, "--trace-from-icount/--trace-from-pc require %s", "--trace");
    if (o->snapshot_at_set && !o->want_snapshot)
        return fail(err, "--snapshot-at requires %s", "--snapshot");
    if (o->show_writes && !o->summary)
        return fail(err, "--show-writes requires %s", "--summary");
    if ((o->monitor_loop_threshold_set || o->monitor_stall_window_set) && !o->monitor)
        return fail(err, "monitor tuning options require %s", "--monitor or --summary");
    if (o->benchmark_json && debug_mode)
        return fail(err, "--benchmark-json cannot be combined with %s", "debugger mode");
    if (o->benchmark_json && o->summary)
        return fail(err, "--benchmark-json cannot be combined with %s", "--summary");
    int managed_gdb = diagnostics &&
                      (diagnostics->capabilities & EMU_DIAG_MANAGED_GDB);
    if (debug_mode && !managed_gdb &&
        (o->summary || o->want_drcov || o->want_snapshot || o->want_dump_flash))
        return fail(err, "debugger mode cannot be combined with %s",
                    o->summary ? "--summary" :
                    o->want_drcov ? "--drcov" :
                    o->want_snapshot ? "--snapshot" : "--dump-flash");
#else
    (void)err;
#endif

    if (o->eeprom_overlay_path && o->from_snapshot)
        return fail(err, "--eeprom-overlay cannot be combined with %s", "--from-snapshot");
    if (o->fsn_set && o->from_snapshot)
        return fail(err, "--fsn cannot be combined with %s", "--from-snapshot");
    if (o->fsn_set && o->eeprom_overlay_path)
        return fail(err, "--fsn cannot be combined with %s", "--eeprom-overlay");
    if (o->imei && !o->fsn_set)
        return fail(err, "--imei requires %s", "--fsn");
    if (o->imei && o->from_snapshot)
        return fail(err, "--imei cannot be combined with %s", "--from-snapshot");
    if (o->imei && o->eeprom_overlay_path)
        return fail(err, "--imei cannot be combined with %s", "--eeprom-overlay");
    return 0;
}

void emu_cli_usage(
        FILE *out, const char *prog,
        const emu_engine_diagnostics_t *diagnostics) {
    fprintf(out,
            "usage: %s <fullflash.bin> [options]\n"
            "       %s --from-snapshot DIR [options]\n\n"
            "Boot a PMB7850 handset fullflash or resume a snapshot.\n",
            prog, prog);
    if (diagnostics &&
        (diagnostics->capabilities & ~EMU_DIAG_SNAPSHOT_RESTORE))
        fputs("Tracing, monitoring, artifacts, and debugging are engine capability-gated.\n",
              out);
    fputc('\n', out);
    const char *last_group = NULL;
    for (size_t i = 0; i < sizeof OPTIONS / sizeof OPTIONS[0]; i++) {
        const option_desc_t *d = &OPTIONS[i];
        if (d->required &&
            (!diagnostics || !(diagnostics->capabilities & d->required)))
            continue;
        if (!last_group || strcmp(last_group, d->group)) {
            fprintf(out, "%s:\n", d->group);
            last_group = d->group;
        }
        char names[96];
        if (d->short_name)
            snprintf(names, sizeof names, "  %s, %s%s%s", d->short_name, d->long_name,
                     d->value_name ? " " : "", d->value_name ? d->value_name : "");
        else
            snprintf(names, sizeof names, "  %s%s%s", d->long_name,
                     d->value_name ? " " : "", d->value_name ? d->value_name : "");
        fprintf(out, "%-34s %s\n", names, d->help);
    }
}
