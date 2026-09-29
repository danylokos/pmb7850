#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "emu_runtime.h"
#include "emu_runtime_state.h"

static int synthetic_parse(unsigned *mask, const char *csv, const char **bad) {
    if (!strcmp(csv, "known")) { *mask |= 2u; return 0; }
    if (bad) *bad = csv;
    return -1;
}

static char *stream_text(FILE *stream) {
    EMU_CHECK(fflush(stream) == 0);
    EMU_CHECK(fseek(stream, 0, SEEK_END) == 0);
    long size = ftell(stream);
    EMU_CHECK(size >= 0 && fseek(stream, 0, SEEK_SET) == 0);
    char *text = malloc((size_t)size + 1u);
    EMU_CHECK(text != NULL);
    EMU_CHECK(fread(text, 1, (size_t)size, stream) == (size_t)size);
    text[size] = 0;
    return text;
}

static void test_cli(void) {
    emu_engine_diagnostics_t instrumented = {
        .engine_name = "fake",
        .profile_name = "instrumented",
        .capabilities =
            EMU_DIAG_MONITOR | EMU_DIAG_ARTIFACTS | EMU_DIAG_DEBUGGER |
            EMU_DIAG_SNAPSHOT | EMU_DIAG_COVERAGE | EMU_DIAG_RAW_LCD |
            EMU_DIAG_SNAPSHOT_RESTORE,
        .trace_available = 1,
        .synthetic_defaults = 1u,
        .synthetic_parse = synthetic_parse,
    };
    emu_cli_options_t options;
    char *error_text = NULL;
    emu_cli_options_init(&options, &instrumented);
    EMU_CHECK(options.limit == UINT64_C(8000000));
    EMU_CHECK(options.synth_mask == 1u);
    char *args[] = {
        "cemu_inst", "flash.bin", "--limit", "2m", "--synth", "known",
        "--patch", "c55-sqwe3", "--trace=exec,lcd", "--summary",
        "--snapshot", "--snapshot-at", "4k",
    };
    FILE *errors = tmpfile();
    EMU_CHECK(errors != NULL);
    EMU_CHECK(emu_cli_parse(&options,
                  (int)(sizeof args / sizeof args[0]), args, errors,
                  &instrumented) == 0);
    EMU_CHECK(options.limit == UINT64_C(2000000));
    EMU_CHECK(options.synth_mask == 3u && options.monitor && options.summary);
    EMU_CHECK(options.want_trace && options.want_snapshot &&
              options.snapshot_at == 4000);
    EMU_CHECK(options.firmware_patches != 0);
    fclose(errors);

    emu_cli_options_init(&options, &instrumented);
    char *identity_args[] = {
        "cemu_inst", "flash.bin", "--fsn", "C8AAE55F",
        "--imei", "35202600729559",
    };
    errors = tmpfile();
    EMU_CHECK(errors != NULL);
    EMU_CHECK(emu_cli_parse(&options,
                  (int)(sizeof identity_args / sizeof identity_args[0]),
                  identity_args, errors, &instrumented) == 0);
    EMU_CHECK(options.fsn_set && options.fsn == 0xC8AAE55Fu);
    EMU_CHECK(!strcmp(options.imei, "35202600729559"));
    fclose(errors);

    emu_cli_options_init(&options, &instrumented);
    char *imei_only[] = {"cemu_inst", "flash.bin", "--imei", "35202600729559"};
    errors = tmpfile();
    EMU_CHECK(errors != NULL);
    EMU_CHECK(emu_cli_parse(&options, 4, imei_only, errors, &instrumented) == -1);
    error_text = stream_text(errors);
    EMU_CHECK(strstr(error_text, "--imei requires --fsn") != NULL);
    free(error_text); fclose(errors);

    const char *modes[] = {"--interactive", "--command", "--script"};
    for (unsigned mode = 0; mode < 3; mode++) {
        for (unsigned transports = 1; transports <= 3; transports++) {
            char *args[9] = {"emu", "flash.bin", (char *)modes[mode]};
            int argc = 3;
            if (mode) args[argc++] = mode == 1 ? "regs; quit" : "commands.txt";
            if (transports & 1) { args[argc++] = "--ui-socket"; args[argc++] = "/tmp/ui"; }
            if (transports & 2) { args[argc++] = "--serial-pty"; args[argc++] = "/tmp/serial"; }
            emu_cli_options_init(&options, &instrumented);
            errors = tmpfile();
            EMU_CHECK(emu_cli_parse(&options, argc, args, errors, &instrumented) == 0);
            EMU_CHECK(!!options.ui_socket_path == !!(transports & 1));
            EMU_CHECK(!!options.serial_pty_path == !!(transports & 2));
            fclose(errors);
        }
    }

    emu_engine_diagnostics_t plain = {
        .engine_name = "fake",
        .profile_name = "plain",
        .synthetic_parse = synthetic_parse,
    };
    emu_cli_options_init(&options, &plain);
    char *bad_args[] = {"cemu", "flash.bin", "--trace"};
    errors = tmpfile();
    EMU_CHECK(emu_cli_parse(&options, 3, bad_args, errors, &plain) == -1);
    error_text = stream_text(errors);
    EMU_CHECK(!strcmp(error_text, "error: unknown argument: --trace\n"));
    free(error_text); fclose(errors);

    FILE *help = tmpfile();
    EMU_CHECK(help != NULL);
    emu_cli_usage(help, "cemu", &plain);
    char *plain_help = stream_text(help);
    EMU_CHECK(strstr(plain_help, "--benchmark-json"));
    EMU_CHECK(strstr(plain_help, "--gdb"));
    EMU_CHECK(!strstr(plain_help, "--monitor"));
    free(plain_help); fclose(help);
    help = tmpfile();
    emu_cli_usage(help, "cemu_inst", &instrumented);
    char *instrumented_help = stream_text(help);
    EMU_CHECK(strstr(instrumented_help, "engine capability-gated"));
    EMU_CHECK(strstr(instrumented_help, "--monitor"));
    free(instrumented_help); fclose(help);

    emu_cli_options_init(&options, &plain);
    char *gdb_args[] = {"emu", "flash.bin", "--gdb", "1234"};
    errors = tmpfile();
    EMU_CHECK(errors != NULL);
    EMU_CHECK(emu_cli_parse(&options, 4, gdb_args, errors, &plain) == 0);
    EMU_CHECK(options.gdb_enabled && options.gdb_port == 1234u);
    fclose(errors);

    const char *bad_ports[] = {"0", "65536", "0x4d2", "12k"};
    for (size_t i = 0; i < sizeof bad_ports / sizeof bad_ports[0]; i++) {
        emu_cli_options_init(&options, &plain);
        char *bad_gdb_args[] = {
            "emu", "flash.bin", "--gdb", (char *)bad_ports[i],
        };
        errors = tmpfile();
        EMU_CHECK(errors != NULL);
        EMU_CHECK(emu_cli_parse(
            &options, 4, bad_gdb_args, errors, &plain) == -1);
        error_text = stream_text(errors);
        EMU_CHECK(strstr(error_text, "GDB port must be 1..65535"));
        free(error_text);
        fclose(errors);
    }

    emu_cli_options_init(&options, &plain);
    char *missing_gdb_port[] = {"emu", "flash.bin", "--gdb"};
    errors = tmpfile();
    EMU_CHECK(errors != NULL);
    EMU_CHECK(emu_cli_parse(
        &options, 3, missing_gdb_port, errors, &plain) == -1);
    error_text = stream_text(errors);
    EMU_CHECK(strstr(error_text, "missing value for --gdb"));
    free(error_text);
    fclose(errors);

    emu_engine_diagnostics_t qemu = {
        .engine_name = "qemu", .profile_name = "supervised",
        .capabilities = EMU_DIAG_MANAGED_GDB | EMU_DIAG_ARTIFACTS,
    };
    for (unsigned flags = 0; flags < 16; flags++) {
        char *args[24] = {"emu", "flash with spaces.bin"};
        int argc = 2;
        if (flags & 1) args[argc++] = "-i";
        if (flags & 2) {
            args[argc++] = "-c"; args[argc++] = "break *0x800100";
            args[argc++] = "--script"; args[argc++] = "commands with spaces.gdb";
            args[argc++] = "--command"; args[argc++] = "continue; literal";
        }
        if (flags & 4) { args[argc++] = "--gdb"; args[argc++] = "4321"; }
        if (flags & 8) { args[argc++] = "--gdb-binary"; args[argc++] = "/gdb with spaces"; }
        args[argc++] = "--dump-flash";
        emu_cli_options_init(&options, &qemu);
        errors = tmpfile();
        EMU_CHECK(emu_cli_parse(&options, argc, args, errors, &qemu) == 0);
        EMU_CHECK(options.dbg_interactive == !!(flags & 1));
        EMU_CHECK(options.gdb_action_count == ((flags & 2) ? 3u : 0u));
        EMU_CHECK(options.gdb_enabled == !!(flags & 4));
        EMU_CHECK(!!options.gdb_binary == !!(flags & 8));
        if (flags & 2) {
            EMU_CHECK(!options.gdb_actions[0].script);
            EMU_CHECK(!strcmp(options.gdb_actions[0].value, "break *0x800100"));
            EMU_CHECK(options.gdb_actions[1].script);
            EMU_CHECK(!strcmp(options.gdb_actions[1].value, "commands with spaces.gdb"));
            EMU_CHECK(!options.gdb_actions[2].script);
            EMU_CHECK(!strcmp(options.gdb_actions[2].value, "continue; literal"));
        }
        fclose(errors);
    }
    /* Managed GDB never enables CEMU's internal command API. */
    EMU_CHECK(!(qemu.capabilities & EMU_DIAG_DEBUGGER));
    char *many[2 * (EMU_GDB_MAX_ACTIONS + 1) + 1] = {"emu"};
    for (size_t i = 0; i <= EMU_GDB_MAX_ACTIONS; i++) {
        many[2 * i + 1] = "-c";
        many[2 * i + 2] = "info registers";
    }
    emu_cli_options_init(&options, &qemu);
    errors = tmpfile();
    EMU_CHECK(emu_cli_parse(&options, 1 + 2 * EMU_GDB_MAX_ACTIONS,
                            many, errors, &qemu) == 0);
    EMU_CHECK(options.gdb_action_count == EMU_GDB_MAX_ACTIONS);
    emu_cli_options_init(&options, &qemu);
    EMU_CHECK(emu_cli_parse(&options, 3 + 2 * EMU_GDB_MAX_ACTIONS,
                            many, errors, &qemu) == -1);
    fclose(errors);

    char *cemu_commands[] = {"cemu", "-c", "regs", "-c", "quit"};
    emu_cli_options_init(&options, &instrumented);
    errors = tmpfile();
    EMU_CHECK(emu_cli_parse(&options, 5, cemu_commands, errors, &instrumented) == 0);
    EMU_CHECK(!strcmp(options.dbg_command, "quit") && !options.gdb_action_count);
    fclose(errors);
}

static void test_runtime_policy(void) {
    struct timespec start = {2, 900000000}, end = {4, 100000000};
    EMU_CHECK(emu_runtime_elapsed_ns(&start, &end) == UINT64_C(1200000000));
    uint64_t deadlines[] = {180, 0, 150, 250};
    EMU_CHECK(emu_runtime_next_allowance(100, 200, deadlines, 4) == 50);
    EMU_CHECK(emu_runtime_next_allowance(128, 200, deadlines, 4) == 22);
    EMU_CHECK(emu_runtime_ticker_enabled(0, 1));
    EMU_CHECK(!emu_runtime_ticker_enabled(1, 1));
    EMU_CHECK(!strcmp(emu_runtime_classify_status("limit", 0, 1, 0),
                      "halted"));
    EMU_CHECK(!strcmp(emu_runtime_classify_status("ui_error", 0, 0, 0),
                      "ui_error"));
    EMU_CHECK(!strcmp(emu_runtime_classify_status("limit", SIGTERM, 0, 0),
                      "interrupted"));
    EMU_CHECK(emu_runtime_exit_code(0, SIGTERM) == 128 + SIGTERM);
    EMU_CHECK(emu_runtime_exit_code(1, SIGTERM) == 2);

    emu_runtime_reset_stop_signal();
    EMU_CHECK(emu_runtime_install_stop_handlers());
    EMU_CHECK(raise(SIGTERM) == 0);
    EMU_CHECK(emu_runtime_stop_signal() == SIGTERM);
    emu_runtime_reset_stop_signal();
}

static void test_benchmark(void) {
    emu_image_metadata_t image = {0};
    snprintf(image.model, sizeof image.model, "C55");
    snprintf(image.langpack, sizeof image.langpack, "lg1");
    image.software_version = 24;
    image.bcore_software_version = -1;
    image.bcore_software_version_raw = 255;
    image.flash_manufacturer_id = 1;
    image.flash_device_id = 0x227e;
    snprintf(image.flash_vendor, sizeof image.flash_vendor, "AMD");
    snprintf(image.flash_engine, sizeof image.flash_engine, "am29lv640mh");
    snprintf(image.flash_classification, sizeof image.flash_classification,
             "native");
    snprintf(image.flash_file_order, sizeof image.flash_file_order, "single");
    emu_benchmark_record_t record = {
        .status = "limit", .reason = "", .device = "c55",
        .device_source = "auto", .image = &image,
        .firmware_patches = 0, .start_icount = 3, .end_icount = 5,
        .ticks = 2, .guest_instructions = 1, .elapsed_seconds = 0.5,
        .pc = 0x802fc4, .state_digest = 0x1234,
        .interrupt_cache_queries = 7, .interrupt_cache_hits = 6,
        .interrupt_cache_scans = 1, .interrupt_cache_invalidations = 2,
    };
    FILE *output = tmpfile();
    EMU_CHECK(output != NULL);
    emu_benchmark_print(output, &record);
    char *json = stream_text(output);
    EMU_CHECK(strstr(json, "{\"schema\":2,\"status\":\"limit\""));
    EMU_CHECK(strstr(json, "\"flash_secondary_offset\":null"));
    EMU_CHECK(strstr(json, "\"ticks_per_s\":4.000000"));
    EMU_CHECK(strstr(json, "\"state_digest\":\"0000000000001234\""));
    EMU_CHECK(json[strlen(json) - 1u] == '\n');
    free(json); fclose(output);
}

static void state_event(void *opaque, const emu_runtime_state_t *state,
                         emu_runtime_event_t event) {
    unsigned *counts = opaque;
    counts[event]++;
    emu_runtime_snapshot_t copy;
    emu_runtime_state_snapshot(state, &copy);
    if (event == EMU_RUNTIME_SNAPSHOT) EMU_CHECK(copy.sample_sequence == counts[event]);
}

static void test_runtime_state(void) {
    emu_runtime_descriptor_t descriptor = {
        .model = "c55", .width = 101, .height = 64, .key_count = 2,
        .keys = {"power", "1"}, .asc0_available = 1, .audio_available = 1,
    };
    uint8_t identity[16] = {1, 2, 3}, copied[16];
    emu_runtime_state_t *state = emu_runtime_state_create(&descriptor, identity);
    EMU_CHECK(state != NULL);
    unsigned counts[2] = {0};
    emu_runtime_state_subscribe(state, state_event, counts);
    descriptor.width = 5;
    strcpy(descriptor.keys[0], "modified");
    emu_runtime_state_descriptor(state, &descriptor);
    EMU_CHECK(descriptor.width == 101 && !strcmp(descriptor.keys[0], "power"));
    emu_runtime_state_identity(state, copied);
    EMU_CHECK(!memcmp(copied, identity, 16));
    emu_runtime_lifecycle_t lifecycle;
    emu_runtime_state_lifecycle(state, &lifecycle);
    EMU_CHECK(lifecycle.phase == EMU_RUNTIME_INITIALIZED);
    emu_runtime_snapshot_t snapshot = {.icount = UINT64_C(9007199254740999),
        .ticks = 0, .guest_instructions = 0, .measured_ns = 100, .pc = 0x123456};
    EMU_CHECK(emu_runtime_state_publish(state, &snapshot, 1) == 0);
    EMU_CHECK(emu_runtime_state_transition(state, EMU_RUNTIME_RUNNING, "", "") == 0);
    EMU_CHECK(emu_runtime_state_transition(state, EMU_RUNTIME_RUNNING, "", "") == -1);
    snapshot.icount += 5;
    snapshot.ticks = 5;
    snapshot.guest_instructions = 3;
    snapshot.measured_ns = 101;
    EMU_CHECK(emu_runtime_state_publish(state, &snapshot, 1) == 0);
    emu_runtime_state_snapshot(state, &snapshot);
    EMU_CHECK(snapshot.icount == UINT64_C(9007199254741004) && snapshot.sample_sequence == 2);
    snapshot.measured_ns = 99;
    EMU_CHECK(emu_runtime_state_publish(state, &snapshot, 1) == -1);
    snapshot.measured_ns = 200;
    snapshot.icount = 0;
    EMU_CHECK(emu_runtime_state_publish(state, &snapshot, 1) == -1);
    EMU_CHECK(emu_runtime_state_rebase(state, &snapshot) == 0);
    emu_runtime_state_snapshot(state, &snapshot);
    EMU_CHECK(snapshot.sample_sequence == 3 && snapshot.icount == 0 &&
              snapshot.ticks == 5 && !snapshot.rates_valid);
    snapshot.measured_ns += UINT64_C(2000000000);
    EMU_CHECK(emu_runtime_state_publish(state, &snapshot, 0) == 0);
    emu_runtime_state_snapshot(state, &snapshot);
    EMU_CHECK(snapshot.rates_valid && snapshot.ticks_per_s == 0 &&
              snapshot.guest_instructions_per_s == 0);
    EMU_CHECK(emu_runtime_state_transition(state, EMU_RUNTIME_STOPPED, "limit", "bound") == 0);
    EMU_CHECK(emu_runtime_state_publish(state, &snapshot, 1) == -1);
    emu_runtime_state_lifecycle(state, &lifecycle);
    EMU_CHECK(lifecycle.phase == EMU_RUNTIME_STOPPED && !strcmp(lifecycle.status, "limit") &&
              !strcmp(lifecycle.reason, "bound") && counts[EMU_RUNTIME_LIFECYCLE] == 2);
    emu_runtime_state_identity(state, copied);
    EMU_CHECK(!memcmp(copied, identity, 16));
    emu_runtime_state_destroy(state);
    /* A resumed guest's absolute counter never supplies invocation identity. */
    state = emu_runtime_state_create(&descriptor, NULL);
    emu_runtime_state_t *resumed = emu_runtime_state_create(&descriptor, NULL);
    EMU_CHECK(state && resumed);
    emu_runtime_state_identity(state, identity);
    emu_runtime_state_identity(resumed, copied);
    EMU_CHECK(memcmp(copied, identity, 16));
    snapshot = (emu_runtime_snapshot_t){
        .icount = UINT64_MAX - 1, .ticks = UINT64_MAX - 2,
        .guest_instructions = UINT64_MAX - 3, .elapsed_ns = UINT64_MAX - 4,
        .measured_ns = UINT64_MAX - 5,
    };
    EMU_CHECK(emu_runtime_state_publish(resumed, &snapshot, 1) == 0);
    emu_runtime_state_snapshot(resumed, &snapshot);
    EMU_CHECK(snapshot.ticks == UINT64_MAX - 2 &&
              snapshot.guest_instructions == UINT64_MAX - 3 &&
              snapshot.elapsed_ns == UINT64_MAX - 4);
    emu_runtime_state_destroy(state);
    emu_runtime_state_destroy(resumed);
}

static void test_measurements(void) {
    const uint64_t second = UINT64_C(1000000000);
    emu_runtime_descriptor_t d = {.model = "c55", .width = 101, .height = 64,
        .key_count = 1, .keys = {"power"}};
    uint8_t id[16] = {0};
    emu_runtime_state_t *s = emu_runtime_state_create(&d, id);
    emu_runtime_snapshot_t v = {.icount = UINT64_MAX - 100000,
        .ticks = UINT64_MAX - 100000, .guest_instructions = UINT64_MAX - 100000,
        .measured_ns = UINT64_MAX - 100 * second};
    emu_runtime_snapshot_t out;
    EMU_CHECK(emu_runtime_state_publish(s, &v, 1) == 0);
    uint64_t start = v.measured_ns;
    v.measured_ns += 9999999;
    EMU_CHECK(emu_runtime_state_publish(s, &v, 0) == 1);
    v.measured_ns++;
    EMU_CHECK(emu_runtime_state_publish(s, &v, 0) == 0);
    for (unsigned i = 2; i <= 200; i++) {
        v.measured_ns = start + i * UINT64_C(10000000);
        v.ticks += 10; v.icount += 10; v.guest_instructions += 5;
        EMU_CHECK(emu_runtime_state_publish(s, &v, 0) == 0);
        emu_runtime_state_snapshot(s, &out);
        EMU_CHECK(out.rates_valid == (i >= 100));
        if (i > 100) {
            EMU_CHECK(out.window_ns == second && out.ticks_per_s == 1000 &&
                      out.guest_instructions_per_s == 500);
        }
    }
    // Sustained slowdown, including guest IDLE and finally no tick progress.
    for (unsigned i = 0; i < 100; i++) {
        v.measured_ns += 10000000; v.ticks++; v.icount++;
        EMU_CHECK(emu_runtime_state_publish(s, &v, 0) == 0);
    }
    emu_runtime_state_snapshot(s, &out);
    EMU_CHECK(out.ticks_per_s == 100 && out.guest_instructions_per_s == 0);
    v.measured_ns += 3 * second; v.ticks += 300; v.icount += 300;
    EMU_CHECK(emu_runtime_state_publish(s, &v, 0) == 0);
    emu_runtime_state_snapshot(s, &out);
    EMU_CHECK(out.window_ns == 3 * second && out.ticks_per_s == 100);
    v.measured_ns += second + 123;
    EMU_CHECK(emu_runtime_state_publish(s, &v, 0) == 0);
    emu_runtime_state_snapshot(s, &out);
    EMU_CHECK(out.window_ns == second + 123 && out.rates_valid && out.ticks_per_s == 0);
    uint64_t seq = out.sample_sequence;
    EMU_CHECK(emu_runtime_state_publish(s, &v, 0) == 1);
    // Forced final publication is legal at a duplicate timestamp.
    EMU_CHECK(emu_runtime_state_publish(s, &v, 1) == 0);
    emu_runtime_state_snapshot(s, &out);
    EMU_CHECK(out.sample_sequence == seq + 1 && out.rates_valid);
    v.ticks--;
    EMU_CHECK(emu_runtime_state_publish(s, &v, 1) == -1);
    v.ticks++; v.guest_instructions--;
    EMU_CHECK(emu_runtime_state_publish(s, &v, 1) == -1);
    v.guest_instructions++; v.icount--;
    EMU_CHECK(emu_runtime_state_publish(s, &v, 1) == -1);
    emu_runtime_state_destroy(s);
    // A replacement/resumed guest has fresh invocation history, no socket needed.
    s = emu_runtime_state_create(&d, id);
    EMU_CHECK(emu_runtime_state_publish(s, &v, 1) == 0);
    emu_runtime_state_snapshot(s, &out);
    EMU_CHECK(!out.rates_valid && !out.window_ns && !out.ticks_per_s);
    // Irregular endpoints select the newest sample at or before the cutoff.
    v.measured_ns += 700000000; v.ticks += 7;
    EMU_CHECK(emu_runtime_state_publish(s, &v, 0) == 0);
    v.measured_ns += 800000000; v.ticks += 8;
    EMU_CHECK(emu_runtime_state_publish(s, &v, 0) == 0);
    emu_runtime_state_snapshot(s, &out);
    EMU_CHECK(out.window_ns == 1500000000 && out.ticks_per_s == 10);
    v.measured_ns += 300000000; v.ticks += 3;
    EMU_CHECK(emu_runtime_state_publish(s, &v, 0) == 0);
    emu_runtime_state_snapshot(s, &out);
    EMU_CHECK(out.window_ns == 1100000000 && out.ticks_per_s == 10);
    emu_runtime_state_destroy(s);

    EMU_CHECK(emu_runtime_sample_allowance(0, 0, 0) == 50000);
    EMU_CHECK(emu_runtime_sample_allowance(100000, 10000000, 0) == 100000);
    EMU_CHECK(emu_runtime_sample_allowance(100000, 10000000, 5000000) == 50000);
    EMU_CHECK(emu_runtime_sample_allowance(UINT64_MAX, 1, 0) == 500000);
    EMU_CHECK(emu_runtime_sample_allowance(1, second, 0) == 1);
    EMU_CHECK(emu_runtime_sample_allowance(1, 1, 10000000) == 1);
}

static void test_ticker(void) {
    emu_runtime_descriptor_t descriptor = {
        .model = "c55", .width = 101, .height = 64,
        .key_count = 1, .keys = {"1"},
    };
    const uint8_t id[16] = {0};
    emu_runtime_state_t *runtime = emu_runtime_state_create(&descriptor, id);
    EMU_CHECK(runtime != NULL);
    EMU_CHECK(emu_runtime_state_transition(runtime, EMU_RUNTIME_RUNNING, "", "") == 0);
    emu_runtime_snapshot_t sample = {.measured_ns = 1000000000, .pc = 0x123456};
    EMU_CHECK(emu_runtime_state_publish(runtime, &sample, 1) == 0);
    emu_runtime_ticker_t slow = {.interval_ns = 100000000}, fast = {.interval_ns = 50000000};
    FILE *out = tmpfile();
    EMU_CHECK(out != NULL);
    EMU_CHECK(emu_runtime_ticker_refresh(out, &slow, runtime, 0, 0) == 1);
    char *text = stream_text(out);
    EMU_CHECK(!strcmp(text, "\r\033[K  running: 0.00s  0.0M ticks  -- ticks/s  -- guest instr/s  pc 0x123456\r"));
    free(text);
    EMU_CHECK(emu_runtime_ticker_refresh(out, &fast, runtime, 0, 0) == 1);
    for (uint64_t i = 1; i <= 100; i++) {
        sample.measured_ns += 10000000;
        sample.elapsed_ns += 10000000;
        sample.ticks += 20000;
        sample.guest_instructions += 10000;
        EMU_CHECK(emu_runtime_state_publish(runtime, &sample, 0) == 0);
        int slow_draw = emu_runtime_ticker_refresh(out, &slow, runtime, i * 10000000, 0);
        int fast_draw = emu_runtime_ticker_refresh(out, &fast, runtime, i * 10000000, 0);
        EMU_CHECK(slow_draw == (i % 10 == 0));
        EMU_CHECK(fast_draw == (i % 5 == 0));
        if (slow_draw) EMU_CHECK(slow.snapshot.sample_sequence == i + 1);
        if (fast_draw) EMU_CHECK(fast.snapshot.sample_sequence == i + 1);
    }
    EMU_CHECK(slow.snapshot.rates_valid && slow.snapshot.ticks_per_s == 2000000);
    EMU_CHECK(slow.snapshot.guest_instructions_per_s == 1000000);
    /* Changing producer performance is reflected without consumer calculations. */
    sample.measured_ns += 1000000000;
    sample.elapsed_ns += 1000000000;
    sample.ticks += 8000000;
    EMU_CHECK(emu_runtime_state_publish(runtime, &sample, 0) == 0);
    emu_runtime_ticker_t saved = slow;
    FILE *restore = tmpfile(), *original = tmpfile();
    EMU_CHECK(restore && original);
    emu_runtime_ticker_restore(restore, &slow);
    emu_runtime_ticker_restore(original, &saved);
    char *restored = stream_text(restore), *expected = stream_text(original);
    EMU_CHECK(!strcmp(restored, expected));
    EMU_CHECK(!memcmp(&slow, &saved, sizeof slow));
    free(restored); free(expected); fclose(restore); fclose(original);
    EMU_CHECK(emu_runtime_ticker_refresh(out, &slow, runtime, 1010000000, 0) == 0);
    EMU_CHECK(emu_runtime_ticker_refresh(out, &slow, runtime, 2000000000, 0) == 1);
    EMU_CHECK(slow.snapshot.ticks_per_s == 8000000 && slow.snapshot.guest_instructions_per_s == 0);
    /* Long scheduling gaps select only the current publication. Final bypasses cadence. */
    EMU_CHECK(emu_runtime_state_transition(runtime, EMU_RUNTIME_STOPPED, "limit", "") == 0);
    EMU_CHECK(emu_runtime_ticker_refresh(out, &slow, runtime, 2000000001, 1) == 1);
    EMU_CHECK(slow.lifecycle.phase == EMU_RUNTIME_STOPPED);
    fclose(out);
    out = tmpfile();
    sample = (emu_runtime_snapshot_t){.elapsed_ns = UINT64_MAX, .ticks = UINT64_MAX,
        .pc = 0x000abc, .rates_valid = 1, .ticks_per_s = 0, .guest_instructions_per_s = 1234567};
    emu_runtime_draw_ticker(out, &sample, &slow.lifecycle);
    text = stream_text(out);
    EMU_CHECK(!strcmp(text, "\r\033[K  limit: 18446744073.71s  18446744073709.6M ticks  0.0M ticks/s  1.2M guest instr/s  pc 0x000abc"));
    free(text); fclose(out);
    out = tmpfile();
    sample.ticks_per_s = 1250000;
    sample.guest_instructions_per_s = 1150000;
    emu_runtime_draw_ticker(out, &sample, &slow.lifecycle);
    text = stream_text(out);
    EMU_CHECK(strstr(text, "1.3M ticks/s  1.1M guest instr/s"));
    free(text); fclose(out);
    emu_runtime_state_destroy(runtime);
}

static void test_console_ticker(void) {
    emu_runtime_ticker_t ticker = {.has_displayed = 1};
    ticker.lifecycle.phase = EMU_RUNTIME_RUNNING;
    const char *warning = "Warning: The guest is now late by 20.0 to 30.0 seconds\n";
    FILE *out = tmpfile();
    EMU_CHECK(out);
    emu_runtime_ticker_restore(out, &ticker);
    emu_runtime_ticker_console(out, &ticker, EMU_CONSOLE_DELAY_WARNING, warning, strlen(warning));
    EMU_CHECK(ticker.visible_rows == 2);
    const char *shorter = "Warning: The guest is now late by 2.0 to 3.0 seconds\n";
    emu_runtime_ticker_console(out, &ticker, EMU_CONSOLE_DELAY_WARNING, shorter, strlen(shorter));
    EMU_CHECK(ticker.visible_rows == 2 && strlen(ticker.warning) == strlen(shorter) - 1);
    emu_runtime_ticker_console(out, &ticker, EMU_CONSOLE_OUTPUT, "serial: hello\n", 14);
    EMU_CHECK(ticker.visible_rows == 2);
    char *text = stream_text(out);
    EMU_CHECK(strstr(text, "\r\033[JWarning: The guest is now late by 2.0"));
    EMU_CHECK(strstr(text, "\r\033[Jserial: hello\n\r\033[KWarning:"));
    free(text);
    emu_runtime_ticker_console(out, &ticker, EMU_CONSOLE_OUTPUT, "partial", 7);
    EMU_CHECK(ticker.console_partial && !ticker.visible_rows);
    emu_runtime_ticker_restore(out, &ticker);
    EMU_CHECK(!ticker.visible_rows);
    emu_runtime_ticker_console(out, &ticker, EMU_CONSOLE_OUTPUT, " tail\n", 6);
    EMU_CHECK(!ticker.console_partial && ticker.visible_rows == 2);
    emu_runtime_ticker_finish(out, &ticker);
    EMU_CHECK(!ticker.visible_rows && !ticker.has_displayed && !ticker.warning[0]);
    text = stream_text(out);
    const char *clear = "\r\033[J";
    EMU_CHECK(!strcmp(text + strlen(text) - strlen(clear), clear));
    free(text);
    fclose(out);
    out = tmpfile();
    emu_runtime_ticker_console(out, NULL, EMU_CONSOLE_DELAY_WARNING, shorter, strlen(shorter) - 1);
    text = stream_text(out);
    EMU_CHECK(!strcmp(text, shorter) && !strchr(text, '\033'));
    free(text); fclose(out);
}

static void test_ticker_width(void) {
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    EMU_CHECK(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
    FILE *out = fopen(ptsname(master), "w");
    EMU_CHECK(out);
    struct winsize size = {.ws_row = 24, .ws_col = 120};
    EMU_CHECK(ioctl(fileno(out), TIOCSWINSZ, &size) == 0);
    emu_runtime_ticker_t ticker = {.has_displayed = 1};
    ticker.lifecycle.phase = EMU_RUNTIME_RUNNING;
    const char warning[] = "Warning: The guest is now late by 2.0 to 3.0 seconds\n";
    emu_runtime_ticker_console(out, &ticker, EMU_CONSOLE_DELAY_WARNING,
                               warning, sizeof warning - 1);
    char output[2048];
    ssize_t count = read(master, output, sizeof output - 1);
    EMU_CHECK(count > 0);
    output[count] = 0;
    EMU_CHECK(strstr(output, "2.0 to 3.0 seconds") && strstr(output, "pc 0x000000"));
    size.ws_col = 20;
    EMU_CHECK(ioctl(fileno(out), TIOCSWINSZ, &size) == 0);
    emu_runtime_ticker_restore(out, &ticker);
    count = read(master, output, sizeof output - 1);
    EMU_CHECK(count > 0);
    output[count] = 0;
    EMU_CHECK(strstr(output, "Warning: The guest \r"));
    EMU_CHECK(strstr(output, "  running: 0.00s  0\r"));
    EMU_CHECK(!strstr(output, "seconds") && !strstr(output, "pc 0x"));
    size.ws_col = 120;
    EMU_CHECK(ioctl(fileno(out), TIOCSWINSZ, &size) == 0);
    emu_runtime_ticker_restore(out, &ticker);
    count = read(master, output, sizeof output - 1);
    EMU_CHECK(count > 0);
    output[count] = 0;
    EMU_CHECK(strstr(output, "2.0 to 3.0 seconds") && strstr(output, "pc 0x000000"));
    size.ws_col = 1;
    EMU_CHECK(ioctl(fileno(out), TIOCSWINSZ, &size) == 0);
    emu_runtime_ticker_restore(out, &ticker);
    count = read(master, output, sizeof output - 1);
    EMU_CHECK(count > 0);
    output[count] = 0;
    EMU_CHECK(!strstr(output, "Warning") && !strstr(output, "running"));
    emu_runtime_ticker_finish(out, &ticker);
    EMU_CHECK(!ticker.visible_rows);
    fclose(out);
    close(master);
}

int main(void) {
    test_ticker();
    test_console_ticker();
    test_ticker_width();
    test_measurements();
    test_runtime_state();
    test_cli();
    test_runtime_policy();
    test_benchmark();
    puts("host CLI and shared runtime policy: PASS");
    return 0;
}
