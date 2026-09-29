#define _POSIX_C_SOURCE 200809L

#include "emu_runtime.h"

#include <errno.h>
#include <stdarg.h>
#include <poll.h>
#include <fcntl.h>
#include <termios.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "emu_artifact.h"
#include "emu_gdb.h"
#include "emu_drcov.h"
#include "emu_manifest.h"
#include "emu_prepare.h"
#include "emu_product.h"
#include "emu_serial.h"
#include "emu_trace.h"
#include "emu_ui.h"

typedef struct {
    int diagnostic_failed;
    FILE *console_out;
    emu_runtime_ticker_t *ticker;
    emu_serial_history_t *serial;
    emu_serial_subscription_t console_subscription, pty_subscription;
    emu_ui_socket_t *ui;
    int ui_open;
    int serial_failed;
    int frame_failed;
    int audio_failed;
    emu_image_capture_t *capture;
    int capture_open;
    int capture_failed;
    int capture_error_reported;
} emu_runtime_callbacks_t;

static void runtime_console_error(emu_runtime_callbacks_t *, const char *, ...);

typedef struct {
    emu_session_t *session;
    emu_runtime_callbacks_t *callbacks;
    int *failed;
    emu_ui_engine_t *engine;
    emu_trace_sink_t **sink;
    int *armed;
} runtime_input_binding_t;
static int runtime_input_set(void *opaque, const char *name, int down) {
    runtime_input_binding_t *b = opaque;
    return b->engine->key_set(b->engine->opaque, name, down);
}
static int runtime_input_sampled(void *opaque, const char *name, int *sampled) {
    runtime_input_binding_t *b = opaque;
    return b->engine->key_sampled(b->engine->opaque, name, sampled);
}
static void runtime_service_trace(runtime_input_binding_t *b, const emu_trace_event_t *event) {
    if (*b->failed) return;
    emu_error_t error = {0};
    emu_error_code_t code = emu_session_trace_host_event(b->session, event, &error);
    if (code == EMU_ERR_UNSUPPORTED) emu_trace_emit(*b->sink, event);
    else if (code != EMU_OK) {
        runtime_console_error(b->callbacks, "error: %s\n", error.message);
        *b->failed = 1;
    }
}
static void runtime_input_trace(void *opaque, const emu_input_event_t *input) {
    runtime_input_binding_t *b = opaque;
    if (!*b->sink || !*b->armed) return;
    int deferred = !strcmp(input->action, "queued") || !strcmp(input->action, "sampled") ||
                   !strcmp(input->action, "cancelled") || !strcmp(input->action, "forced");
    const char *kind = deferred ? "keypad_deferred_release" : "input_owner";
    if (!emu_trace_sink_accepts(*b->sink, kind)) return;
    char owner[32];
    snprintf(owner, sizeof owner, "%llu", (unsigned long long)input->owner);
    emu_trace_event_t e = {.kind = kind, .icount = input->icount, .pc = input->pc,
                           .detail = input->action};
    emu_trace_info_str(&e.info, "button", input->name);
    emu_trace_info_bool(&e.info, "changed", input->changed);
    emu_trace_info_bool(&e.info, "down", input->down);
    emu_trace_info_int(&e.info, "holders", input->holders);
    emu_trace_info_int(&e.info, "key_index", input->key);
    emu_trace_info_str(&e.info, "owner", owner);
    emu_trace_info_bool(&e.info, "pending", input->pending);
    emu_trace_info_str(&e.info, "phase", input->action);
    emu_trace_info_int(&e.info, "result", input->result);
    runtime_service_trace(b, &e);
}

static void runtime_serial_trace(void *opaque, const emu_serial_event_t *serial) {
    runtime_input_binding_t *b = opaque;
    if (!*b->sink || !*b->armed || !emu_trace_sink_accepts(*b->sink, "serial_history")) return;
    emu_trace_event_t e = {.kind = "serial_history", .icount = serial->icount,
        .pc = serial->pc, .detail = serial->action};
    emu_trace_info_str(&e.info, "action", serial->action);
    emu_trace_info_bool(&e.info, "changed", serial->changed);
    emu_trace_info_int(&e.info, "result", serial->result);
    const char *names[] = {"subscription", "start", "end", "boundary", "cursor", "tail"};
    uint64_t values[] = {serial->subscription, serial->start, serial->end,
                        serial->boundary, serial->cursor, serial->tail};
    char text[6][32];
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        snprintf(text[i], sizeof text[i], "%llu", (unsigned long long)values[i]);
        emu_trace_info_str(&e.info, names[i], text[i]);
    }
    runtime_service_trace(b, &e);
}

static ptrdiff_t runtime_text_read(void *opaque, char *bytes, size_t size) {
    FILE *file = opaque;
    if (!file || (!bytes && size)) return -1;
    ssize_t count = read(fileno(file), bytes, size);
    return count < 0 && (errno == EAGAIN || errno == EINTR) ? -2 : count;
}

static int runtime_text_wait(void *opaque, int timeout_ms) {
    struct pollfd fd = {.fd = fileno(opaque), .events = POLLIN};
    int ready = poll(&fd, 1, timeout_ms);
    if (ready < 0 && errno == EINTR) return 0;
    if (ready < 0 || (fd.revents & (POLLERR | POLLNVAL))) return -1;
    return ready > 0;
}

static int runtime_text_write(
        void *opaque, const char *bytes, size_t size) {
    FILE *file = opaque;
    return file && fwrite(bytes, 1, size, file) == size && fflush(file) == 0 ? 0 : -1;
}

static char *runtime_read_text_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    size_t size = 0, capacity = 4096u;
    char *text = malloc(capacity);
    if (!text) {
        fclose(file);
        return NULL;
    }
    for (;;) {
        if (size + 1u == capacity) {
            if (capacity > SIZE_MAX / 2u) {
                free(text);
                fclose(file);
                return NULL;
            }
            capacity *= 2u;
            char *resized = realloc(text, capacity);
            if (!resized) {
                free(text);
                fclose(file);
                return NULL;
            }
            text = resized;
        }
        size_t count = fread(text + size, 1, capacity - size - 1u, file);
        size += count;
        if (count == 0) break;
    }
    int failed = ferror(file) || fclose(file) != 0;
    if (failed) {
        free(text);
        return NULL;
    }
    text[size] = 0;
    return text;
}

static int runtime_snapshot_provenance(
        const emu_prepared_session_t *prepared,
        emu_patch_set_t selected, emu_patch_set_t inherited,
        emu_patch_set_t effective, char *output, size_t capacity,
        emu_error_t *error) {
    if (!prepared || !output || !capacity ||
        (selected | inherited) != effective ||
        prepared->options.firmware_patches != effective ||
        !prepared->source.locator ||
        strpbrk(prepared->source.locator, "\"\\\r\n") ||
        emu_prepared_rehash_source(prepared, error) != EMU_OK)
        return -1;
    int used = snprintf(output, capacity,
        "    \"flash\": \"%s\",\n    \"firmware_patches\": [",
        prepared->source.locator);
    if (used < 0 || (size_t)used >= capacity) return -1;
    size_t count = 0;
    const emu_patch_definition_t *catalog = emu_patch_catalog(&count);
    int first = 1;
    for (size_t i = 0; i < count; i++) {
        if (!(effective & (UINT64_C(1) << i))) continue;
        int written = snprintf(
            output + used, capacity - (size_t)used, "%s\"%s\"",
            first ? "" : ", ", catalog[i].name);
        if (written < 0 || (size_t)written >= capacity - (size_t)used)
            return -1;
        used += written;
        first = 0;
    }
    int written = snprintf(output + used, capacity - (size_t)used, "],\n");
    return written >= 0 && (size_t)written < capacity - (size_t)used
         ? 0 : -1;
}

static int runtime_write_flash(
        emu_session_t *session, const char *path, emu_error_t *error) {
    size_t flash_size = 0;
    if (!session || !path || !*path ||
        emu_session_flash_size(session, &flash_size, error) != EMU_OK ||
        !flash_size)
        return -1;
    char temporary[EMU_ARTIFACT_PATH_MAX + 48u];
    int length = snprintf(temporary, sizeof temporary, "%s.tmp.%ld",
                          path, (long)getpid());
    if (length < 0 || (size_t)length >= sizeof temporary) return -1;
    FILE *file = fopen(temporary, "wb");
    if (!file) return -1;
    uint8_t *buffer = malloc(64u * 1024u);
    int failed = buffer == NULL;
    for (size_t offset = 0; !failed && offset < flash_size;) {
        size_t size = flash_size - offset;
        if (size > 64u * 1024u) size = 64u * 1024u;
        if (emu_session_flash_read(
                session, offset, buffer, size, error) != EMU_OK ||
            fwrite(buffer, 1, size, file) != size)
            failed = 1;
        offset += size;
    }
    free(buffer);
    if (fclose(file) != 0) failed = 1;
    if (!failed && rename(temporary, path) != 0) failed = 1;
    if (failed) (void)remove(temporary);
    return failed ? -1 : 0;
}

static int runtime_file_identity(const char *path, uint64_t *size,
                                 char sha256[EMU_SHA256_HEX_SIZE]) {
    struct stat status;
    char dataset_manifest[EMU_ARTIFACT_PATH_MAX * 2u + 32u];
    if (!path || stat(path, &status)) return -1;
    const char *identity_path = path;
    if (S_ISDIR(status.st_mode)) {
        int length = snprintf(dataset_manifest, sizeof dataset_manifest,
                              "%s/manifest.json", path);
        if (length < 0 || (size_t)length >= sizeof dataset_manifest ||
            stat(dataset_manifest, &status)) return -1;
        identity_path = dataset_manifest;
    }
    if (status.st_size < 0 || !S_ISREG(status.st_mode)) return -1;
    FILE *file = fopen(identity_path, "rb");
    if (!file) return -1;
    size_t length = (size_t)status.st_size;
    uint8_t *bytes = malloc(length ? length : 1u);
    int failed = !bytes ||
        (length && fread(bytes, 1, length, file) != length) ||
        fclose(file) != 0;
    if (failed) {
        free(bytes);
        return -1;
    }
    uint8_t digest[EMU_SHA256_SIZE];
    emu_sha256(bytes, length, digest);
    emu_sha256_hex(digest, sha256);
    free(bytes);
    *size = length;
    return 0;
}

static int runtime_write_manifest(
        const emu_artifact_run_t *run,
        const emu_prepared_session_t *prepared,
        const emu_engine_descriptor_t *engine,
        const emu_preparation_request_t *request,
        const emu_preparation_result_t *preparation,
        const emu_run_result_t *result, int explicit_writeback,
        emu_error_t *error) {
    emu_manifest_t manifest;
    emu_manifest_init_v2(&manifest, prepared, engine, result,
                         request, preparation);
    manifest.explicit_writeback = explicit_writeback;
    for (size_t i = 0; i < run->registration_count; i++) {
        const emu_artifact_registration_t *item = &run->registrations[i];
        char path[EMU_ARTIFACT_PATH_MAX * 2u];
        uint64_t size;
        char sha256[EMU_SHA256_HEX_SIZE];
        int length = snprintf(path, sizeof path, "%s/%s",
                              run->path, item->path);
        if (length < 0 || (size_t)length >= sizeof path ||
            runtime_file_identity(path, &size, sha256) != 0 ||
            emu_manifest_add_artifact(
                &manifest, item->kind, item->path, size, sha256,
                error) != EMU_OK)
            return -1;
    }
    char *json = NULL;
    size_t json_size = 0;
    if (emu_manifest_serialize(
            &manifest, &json, &json_size, error) != EMU_OK)
        return -1;
    char path[EMU_ARTIFACT_PATH_MAX + 32u];
    char temporary[EMU_ARTIFACT_PATH_MAX + 64u];
    int path_size = snprintf(path, sizeof path, "%s/manifest.json",
                             run->path);
    int temporary_size = snprintf(
        temporary, sizeof temporary, "%s.tmp.%ld", path, (long)getpid());
    FILE *file = path_size < 0 || (size_t)path_size >= sizeof path ||
                 temporary_size < 0 ||
                 (size_t)temporary_size >= sizeof temporary
               ? NULL : fopen(temporary, "wb");
    int failed = !file;
    if (file) {
        if (fwrite(json, 1, json_size, file) != json_size) failed = 1;
        if (fclose(file) != 0) failed = 1;
    }
    if (!failed && rename(temporary, path) != 0) failed = 1;
    if (failed && temporary[0]) (void)remove(temporary);
    free(json);
    return failed ? -1 : 0;
}

static void runtime_serial(void *opaque, const uint8_t *data, size_t size) {
    emu_runtime_callbacks_t *callbacks = opaque;
    if (!size || callbacks->serial_failed) return;
    if (emu_serial_history_append(callbacks->serial, data, size))
        callbacks->serial_failed = 1;
}

static void runtime_frame(void *opaque, const emu_frame_t *frame) {
    emu_runtime_callbacks_t *callbacks = opaque;
    if (!callbacks->ui_open || callbacks->frame_failed) return;
    if (emu_ui_socket_frame(
            callbacks->ui, frame->width, frame->height,
            frame->rgb, frame->rgb_size, frame->icount) != 0)
        callbacks->frame_failed = 1;
}

static void runtime_audio_output(
        void *opaque, const emu_audio_output_t *output) {
    emu_runtime_callbacks_t *callbacks = opaque;
    if (!callbacks->ui_open || callbacks->audio_failed) return;
    if (emu_ui_socket_speaker_pcm(callbacks->ui, output) != 0)
        callbacks->audio_failed = 1;
}

static void runtime_audio_reset(
        void *opaque, const emu_audio_reset_t *reset) {
    emu_runtime_callbacks_t *callbacks = opaque;
    if (callbacks->ui_open) {
        if (reset->reason == EMU_AUDIO_RESET_SNAPSHOT)
            callbacks->ui->last_render_valid = 0;
        emu_ui_socket_audio_reset(callbacks->ui, reset->icount);
    }
}

static void runtime_capture_frame(void *opaque, const emu_frame_t *frame) {
    emu_runtime_callbacks_t *callbacks = opaque;
    if (!callbacks->capture_open || callbacks->capture_failed) return;
    emu_capture_kind_t kind = frame->kind == EMU_FRAME_DDRAM
                            ? EMU_CAPTURE_RAW_DDRAM
                            : EMU_CAPTURE_STRICT;
    if (emu_image_capture_write(
            callbacks->capture, kind, frame->sequence, frame->icount,
            frame->rgb, frame->width, frame->height) != 0)
        callbacks->capture_failed = 1;
}

static int runtime_close_capture(
        emu_session_t *session, emu_runtime_callbacks_t *callbacks,
        emu_image_capture_t *capture, const emu_cli_options_t *options,
        int report_error) {
    if (!callbacks->capture_open) return callbacks->capture_failed;
    emu_artifact_request_t request = {.action = EMU_ARTIFACT_STOP};
    emu_artifact_result_t result;
    emu_error_t error = {0};
    int failed = callbacks->capture_failed;
    const emu_engine_descriptor_t *engine = emu_session_engine(session);
    int engine_artifacts = engine &&
        (engine->capabilities & EMU_CAP_ARTIFACT_REQ);
    if (options->want_lcd_frames && engine_artifacts) {
        request.kind = EMU_ARTIFACT_LCD_FRAME;
        failed |= emu_session_artifact(
            session, &request, &result, &error) != EMU_OK;
    }
    if (options->want_lcd_ddram_frames && engine_artifacts) {
        request.kind = EMU_ARTIFACT_LCD_DDRAM;
        failed |= emu_session_artifact(
            session, &request, &result, &error) != EMU_OK;
    }
    callbacks->capture_open = 0;
    int was_failed = capture->failed;
    if (emu_image_capture_close(capture) != 0) {
        failed = 1;
        if (report_error && !was_failed)
            runtime_console_error(callbacks, "error: cannot finalize LCD capture GIF\n");
    }
    return failed;
}

static void runtime_console(void *opaque, emu_console_kind_t kind,
                             const char *text, size_t size) {
    emu_runtime_callbacks_t *callbacks = opaque;
    emu_runtime_ticker_console(callbacks->console_out, callbacks->ticker,
                               kind, text, size);
}

static void runtime_console_error(emu_runtime_callbacks_t *callbacks,
                                   const char *format, ...) {
    if (callbacks->ticker) emu_runtime_ticker_clear(stdout, callbacks->ticker);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    if (callbacks->ticker) emu_runtime_ticker_restore(stdout, callbacks->ticker);
}

static int runtime_flush_serial(emu_runtime_callbacks_t *callbacks,
                                int *serial_printed, int force,
                                emu_runtime_ticker_t *ticker) {
    char *escaped = NULL;
    int ready = emu_serial_history_console_take(
        callbacks->serial, callbacks->console_subscription, force, &escaped);
    if (ready <= 0) return ready;
    *serial_printed = 1;
    if (ticker) {
        emu_runtime_ticker_clear(stdout, ticker);
        if (ticker->console_partial) putchar('\n');
        ticker->console_partial = 0;
    }
    printf("serial: %s\n", escaped);
    free(escaped);
    if (ticker)
        emu_runtime_ticker_restore(stdout, ticker);
    else
        fflush(stdout);
    return 1;
}

static void runtime_print_startup(
        const emu_cli_options_t *options,
        const emu_prepared_session_t *prepared,
        const emu_preparation_result_t *preparation,
        uint64_t limit) {
    const emu_image_metadata_t *image = &prepared->metadata;
    printf("image: %s SW%d %s  BCORE SW", image->model,
           image->software_version, image->langpack);
    if (image->bcore_software_version >= 0)
        printf("%d", image->bcore_software_version);
    else
        printf("? (raw %02X)", image->bcore_software_version_raw);
    printf("  metadata-view-offset: 0x%zX\n", image->metadata_view_offset);
    if (strcmp(image->flash_file_order, "single")) {
        printf("flash-file-order: %s  primary-offset: 0x%zX  secondary-offset: 0x%zX\n",
               image->flash_file_order, prepared->chips[0].source_offset,
               prepared->chip_count > 1
                   ? prepared->chips[1].source_offset : 0u);
    }
    printf("flash-id: %s %04X/%04X  engine: %s (%s%s)\n",
           image->flash_vendor, image->flash_manufacturer_id,
           image->flash_device_id, image->flash_engine,
           image->flash_classification,
           !strcmp(image->flash_classification, "compatible")
               ? " emulator approximation" : "");
    printf("device: %s (%s%s%s)  flash: %s (%zu bytes)  ",
           prepared->selected_device,
           options->device_name ? "override; image model "
                                : "auto from image model ",
           image->model, "", preparation->source_path,
           prepared->source.size);
    if (limit == UINT64_MAX)
        printf("limit: unbounded%s%s\n",
               options->ui_socket_path ? "  ui: " : "",
               options->ui_socket_path ? options->ui_socket_path : "");
    else
        printf("limit: %llu%s%s\n", (unsigned long long)limit,
               options->ui_socket_path ? "  ui: " : "",
               options->ui_socket_path ? options->ui_socket_path : "");
}

static void runtime_print_identity(
        const emu_cli_options_t *options,
        const emu_prepared_session_t *prepared,
        const emu_preparation_result_t *preparation) {
    if (!preparation->identity_planned) return;
    const emu_identity_plan_result_t *identity = &preparation->identity;
    if (!identity->fsn_only_fallback && identity->imei[0]) {
        fprintf(stderr,
                "Identity: source=%s FSN=%08X flash=%s, IMEI=%s, %u records seeded\n",
                options->eeprom_overlay_path ? "overlay" : "built-in",
                identity->fsn, prepared->metadata.flash_engine,
                identity->imei, (unsigned)identity->record_count);
    } else if (identity->fsn_only_fallback) {
        fprintf(stderr,
                "Identity: source=built-in FSN=%08X flash=%s, EEPROM directory absent\n",
                identity->fsn, prepared->metadata.flash_engine);
    } else {
        fprintf(stderr,
                "Identity: source=explicit FSN=%08X flash=%s, EEPROM preserved\n",
                identity->fsn, prepared->metadata.flash_engine);
    }
}

typedef struct {
    emu_session_t *session;
    emu_runtime_state_t *runtime;
    emu_input_t *input;
    emu_runtime_callbacks_t *callbacks;
    emu_ui_socket_t *ui;
    emu_serial_pty_t *pty;
    int interactive;
    uint64_t invocation_ns, ticks, guest;
    emu_engine_state_t state;
} runtime_debugger_t;

static int runtime_debugger_service(void *opaque, int phase,
                                     uint64_t ticks, uint64_t guest) {
    runtime_debugger_t *d = opaque;
    emu_runtime_callbacks_t *cb = d->callbacks;
    emu_error_t error = {0};
    d->ticks = ticks;
    d->guest = guest;
    if (emu_session_query_request(d->session, 0, &d->state, &error) != EMU_OK)
        return -1;
    emu_input_context(d->input, d->state.icount, d->state.pc);
    emu_serial_history_context(cb->serial, d->state.icount, d->state.pc);
    uint64_t now = emu_runtime_monotonic_ns();
    emu_runtime_snapshot_t snapshot = {
        .icount = d->state.icount, .pc = d->state.pc,
        .ticks = ticks, .guest_instructions = guest,
        .elapsed_ns = now - d->invocation_ns, .measured_ns = now,
    };
    if (phase == 2) {
        if (emu_input_reconcile(d->input) ||
            emu_runtime_state_rebase(d->runtime, &snapshot)) return -1;
    } else if (emu_runtime_state_publish(d->runtime, &snapshot, 0) < 0)
        return -1;
    /* Querying the sampling latch never executes firmware. Releases remain
     * pending until firmware has observed the press, including after a step. */
    if (phase != 2 && emu_input_poll(d->input)) return -1;
    if (d->pty && emu_serial_history_pty_poll(
            cb->serial, cb->pty_subscription, d->pty)) return -1;
    emu_runtime_state_snapshot(d->runtime, &snapshot);
    if (d->ui && emu_ui_socket_poll(d->ui, &snapshot, 0)) return -1;
    if (cb->diagnostic_failed || cb->serial_failed || cb->frame_failed ||
        cb->audio_failed || cb->capture_failed) return -1;
    int signal_number = emu_runtime_stop_signal();
    if (signal_number == SIGINT && d->interactive) {
        emu_runtime_reset_stop_signal();
        if (phase != 1 && isatty(STDIN_FILENO)) tcflush(STDIN_FILENO, TCIFLUSH);
        return 1;
    }
    return signal_number ? 2 : 0;
}

static int emu_runtime_main(
        int argc, char **argv, const emu_engine_descriptor_t *engine,
        const char *default_qemu_binary, emu_gdb_t *gdb) {
    const emu_engine_diagnostics_t *diagnostics = engine->diagnostics;
    int continuous = engine->execution_model == EMU_EXECUTION_CONTINUOUS;
    emu_cli_options_t options;
    emu_cli_options_init(&options, diagnostics);
    if (emu_cli_parse(&options, argc, argv, stderr, diagnostics) != 0) {
        fputc('\n', stderr);
        emu_cli_usage(stderr, argv[0], diagnostics);
        return 2;
    }
    if (options.help) {
        emu_cli_usage(stdout, argv[0], diagnostics);
        return 0;
    }
    if (!continuous && options.qemu_binary) {
        fprintf(stderr, "error: --qemu-binary requires --engine qemu\n");
        return 2;
    }
    if (!continuous && options.gdb_enabled) {
        fprintf(stderr, "error: --gdb requires --engine qemu\n");
        return 2;
    }
    if (continuous &&
        (options.limit_explicit || options.batch_idle ||
         options.benchmark_json ||
         options.synth_spec ||
         options.want_drcov || options.want_lcd_ddram_frames ||
         options.want_snapshot_full_bins ||
         options.snapshot_at_set || options.trace_from_icount_set ||
         options.trace_from_pc >= 0 || options.summary || options.monitor ||
         options.show_writes)) {
        fprintf(stderr,
                "error: requested run control or diagnostic is unsupported by engine qemu\n");
        return 2;
    }
    if (options.want_trace && diagnostics &&
        (options.trace_mask & ~diagnostics->trace_mask)) {
        fprintf(stderr,
                "error: requested trace selectors are unsupported by engine %s\n",
                engine->name);
        return 2;
    }
    if (options.synth_spec) {
        if (diagnostics->synthetic_print)
            diagnostics->synthetic_print(stdout, options.synth_mask);
        return 0;
    }
    if (options.patch_list) {
        emu_patch_print(stdout);
        return 0;
    }
    if (options.trace_list) {
        fputs(emu_trace_selector_list_text(), stdout);
        return 0;
    }
    if (!options.flash_path && !options.from_snapshot) {
        fprintf(stderr,
                "error: no flash image (give <fullflash.bin> or --from-snapshot DIR)\n\n");
        emu_cli_usage(stderr, argv[0], diagnostics);
        return 2;
    }
    int debug_mode = options.dbg_command || options.dbg_script ||
                     options.dbg_interactive;

    int managed_gdb = debug_mode && diagnostics &&
                      (diagnostics->capabilities & EMU_DIAG_MANAGED_GDB);
    if (options.gdb_binary && !managed_gdb) {
        fprintf(stderr, "error: --gdb-binary requires -i, -c, or --script\n");
        return 2;
    }
    if (managed_gdb) {
        /* Cover QEMU creation too, before either child can outlive a signal. */
        emu_runtime_reset_stop_signal();
        if (!emu_runtime_install_stop_handlers()) {
            fprintf(stderr, "error: cannot install signal handlers\n");
            return 2;
        }
    }

    uint64_t limit = options.limit;
    if (continuous || ((options.ui_socket_path || options.serial_pty_path) &&
                       !options.limit_explicit))
        limit = UINT64_MAX;
    emu_preparation_request_t request = {
        .mode = options.from_snapshot ? EMU_STARTUP_RESUME
                                      : EMU_STARTUP_FRESH,
        .source_path = options.flash_path,
        .requested_device = options.device_name,
        .snapshot_path = options.from_snapshot,
        .identity_source = options.eeprom_overlay_path
                         ? EMU_IDENTITY_SOURCE_FILE
                         : options.fsn_set ? EMU_IDENTITY_SOURCE_FSN
                                           : EMU_IDENTITY_SOURCE_DEFAULT,
        .identity_path = options.eeprom_overlay_path,
        .fsn = options.fsn,
        .imei = options.imei,
        .selected_patches = options.firmware_patches,
        .runtime_options = {
            .synthetic_mask = options.synth_mask,
            .serial_autobaud_bypass = 0,
            .sim_stub = options.sim_stub,
            .trace_mask = options.want_trace ? options.trace_mask : 0,
            .gdb_port = options.gdb_port,
            .gdb_enabled = options.gdb_enabled,
            .snapshot_requested = options.want_snapshot,
        },
    };
    if (managed_gdb && emu_gdb_prepare(gdb, &options, &request.runtime_options))
        return 2;
    if (continuous) {
        const char *binary = options.qemu_binary ? options.qemu_binary
                                                 : default_qemu_binary;
        if (!binary || !binary[0] ||
            snprintf(request.runtime_options.qemu_binary,
                     sizeof request.runtime_options.qemu_binary,
                     "%s", binary) >=
                (int)sizeof request.runtime_options.qemu_binary) {
            fprintf(stderr, "error: invalid QEMU binary path\n");
            return 2;
        }
    }
    emu_prepared_session_t prepared;
    emu_preparation_result_t preparation;
    emu_error_t error = {0};
    if (emu_prepare_startup(
            &request, &prepared, &preparation, &error) != EMU_OK) {
        if (preparation.failed_phase == EMU_PREPARATION_PHASE_PRODUCT)
            fprintf(stderr, "invalid flash image%s%s: %s\n",
                    options.device_name ? " for " : "",
                    options.device_name ? options.device_name
                                        : " during auto-detection",
                    error.message);
        else
            fprintf(stderr, "error: %s\n", error.message);
        return 2;
    }

    emu_runtime_ticker_t terminal = {.interval_ns = UINT64_C(100000000)};
    int ticker = !debug_mode && emu_runtime_ticker_enabled(
        options.benchmark_json, isatty(STDOUT_FILENO));
    emu_runtime_callbacks_t callback_state = {
        .serial = emu_serial_history_create(NULL),
        .console_out = options.benchmark_json ? stderr : stdout,
        .ticker = ticker ? &terminal : NULL,
    };
    if (!callback_state.serial) {
        fprintf(stderr, "error: cannot create serial history\n");
        emu_prepared_free(&prepared);
        return 2;
    }
    callback_state.console_subscription = emu_serial_history_subscribe(callback_state.serial, 0);
    callback_state.pty_subscription = emu_serial_history_subscribe(callback_state.serial, 0);
    if (!callback_state.console_subscription || !callback_state.pty_subscription)
        callback_state.serial_failed = 1;
    emu_callbacks_t callbacks = {
        .opaque = &callback_state,
        .console = runtime_console,
        .frame = runtime_frame,
        .capture_frame = runtime_capture_frame,
        .serial = runtime_serial,
        .audio_output = runtime_audio_output,
        .audio_reset = runtime_audio_reset,
    };
    emu_engine_registry_t registry = {0};
    emu_session_t *session = NULL;
    if (emu_registry_register(&registry, engine, &error) != EMU_OK ||
        emu_session_create(&registry, engine->name, &prepared, &callbacks,
                           &session, &error) != EMU_OK) {
        fprintf(stderr, "error: %s\n", error.message);
        emu_prepared_free(&prepared);
        emu_serial_history_destroy(callback_state.serial);
        return 2;
    }
    if (options.from_snapshot &&
        emu_session_snapshot_restore(
            session, options.from_snapshot, &error) != EMU_OK) {
        fprintf(stderr, "error: %s\n", error.message);
        emu_session_destroy(&session);
        emu_prepared_free(&prepared);
        emu_serial_history_destroy(callback_state.serial);
        return 2;
    }
    runtime_print_identity(&options, &prepared, &preparation);
    if (preparation.effective_patches) {
        char names[256];
        emu_patch_active_names(
            preparation.effective_patches, names, sizeof names);
        fprintf(stderr, "FIRMWARE PATCHES: active [%s]\n", names);
    }
    if (options.battery_level_set || options.battery_charging_set) {
        if (emu_session_battery(
                session, options.battery_level,
                options.battery_charging, &error) != EMU_OK) {
            fprintf(stderr,
                    "error: battery controls are not configured for device %s\n",
                    prepared.selected_device);
            emu_session_destroy(&session);
            emu_prepared_free(&prepared);
            emu_serial_history_destroy(callback_state.serial);
            return 2;
        }
    }
    if ((engine->capabilities & EMU_CAP_SIM) &&
        emu_session_sim(session, prepared.options.sim_stub, &error) != EMU_OK) {
        fprintf(stderr, "error: %s\n", error.message);
        emu_session_destroy(&session);
        emu_prepared_free(&prepared);
        emu_serial_history_destroy(callback_state.serial);
        return 2;
    }
    if (options.synth_mask != diagnostics->synthetic_defaults) {
        char names[128] = "";
        if (diagnostics->synthetic_names)
            diagnostics->synthetic_names(
                options.synth_mask, names, sizeof names);
        fprintf(stderr,
                "SYNTHETIC: active behaviors [%s] (differs from faithful baseline)\n",
                names[0] ? names : "none");
    }

    const emu_product_t *product =
        emu_product_by_name(prepared.selected_device);
    int want_lcd_capture =
        options.want_lcd_frames || options.want_lcd_ddram_frames;
    int want_output = want_lcd_capture || options.want_trace ||
                      options.want_drcov || options.want_snapshot ||
                      options.want_dump_flash;
    emu_artifact_run_t artifact_run = {0};
    emu_image_capture_t image_capture = {0};
    int artifact_open = 0;
    int artifact_failed = 0;
    char lcd_dir[EMU_ARTIFACT_PATH_MAX] = "";
    char raw_dir[EMU_ARTIFACT_PATH_MAX] = "";
    char trace_path[EMU_ARTIFACT_PATH_MAX] = "";
    char drcov_path[EMU_ARTIFACT_PATH_MAX] = "";
    char snapshot_dir[EMU_ARTIFACT_PATH_MAX] = "";
    char flash_path[EMU_ARTIFACT_PATH_MAX] = "";
    if (want_output) {
        char mmdd[8] = "";
        char registered[EMU_ARTIFACT_PATH_MAX];
        emu_artifact_today_mmdd(mmdd, sizeof mmdd);
        int open_failed = !product ||
            emu_artifact_run_open(
                &artifact_run, "shots", prepared.selected_device,
                options.label, mmdd) != 0;
        if (!open_failed && options.want_trace) {
            char directory[EMU_ARTIFACT_PATH_MAX];
            open_failed =
                emu_artifact_run_subdir(
                    &artifact_run, "trace", directory,
                    sizeof directory) != 0 ||
                emu_artifact_run_path(
                    &artifact_run, "trace", "trace/trace.parquet",
                    trace_path, sizeof trace_path) != 0;
        }
        if (!open_failed && options.want_drcov) {
            char directory[EMU_ARTIFACT_PATH_MAX];
            open_failed =
                emu_artifact_run_subdir(
                    &artifact_run, "coverage", directory,
                    sizeof directory) != 0 ||
                emu_artifact_run_path(
                    &artifact_run, "coverage", "coverage/cov.drcov",
                    drcov_path, sizeof drcov_path) != 0;
        }
        if (!open_failed && options.want_snapshot) {
            open_failed =
                emu_artifact_run_subdir(
                    &artifact_run, "snapshot", snapshot_dir,
                    sizeof snapshot_dir) != 0 ||
                emu_artifact_run_path(
                    &artifact_run, "snapshot", "snapshot/snapshot.json",
                    registered, sizeof registered) != 0;
        }
        if (!open_failed && options.want_dump_flash)
            open_failed = emu_artifact_run_path(
                &artifact_run, "flash", "flash.bin", flash_path,
                sizeof flash_path) != 0;
        if (!open_failed && options.want_lcd_frames)
            open_failed =
                emu_artifact_run_subdir(
                    &artifact_run, "lcd", lcd_dir, sizeof lcd_dir) != 0 ||
                emu_artifact_run_path(
                    &artifact_run, "lcd-animation", "lcd/frames.gif",
                    registered, sizeof registered) != 0;
        if (!open_failed && options.want_lcd_ddram_frames)
            open_failed = emu_artifact_run_subdir(
                &artifact_run, "lcd-ddram", raw_dir, sizeof raw_dir) != 0;
        if (!open_failed && want_lcd_capture)
            open_failed = emu_image_capture_open(
                &image_capture, lcd_dir, raw_dir,
                options.want_lcd_frames, options.want_lcd_ddram_frames,
                product ? product->display.width * 4u : 0,
                product ? product->display.height * 4u : 0) != 0;
        if (open_failed) {
            if (want_lcd_capture)
                fprintf(stderr, "error: cannot open LCD capture GIF\n");
            else if (options.want_trace)
                fprintf(stderr, "error: cannot open trace %s\n",
                        trace_path[0] ? trace_path : artifact_run.path);
            else if (options.want_drcov)
                fprintf(stderr, "error: cannot open drcov %s\n",
                        drcov_path[0] ? drcov_path : artifact_run.path);
            else if (options.want_snapshot)
                fprintf(stderr, "error: cannot write snapshot %s\n",
                        snapshot_dir[0] ? snapshot_dir : artifact_run.path);
            else
                fprintf(stderr, "error: cannot write flash image %s\n",
                        flash_path[0] ? flash_path : artifact_run.path);
            emu_session_destroy(&session);
            emu_prepared_free(&prepared);
            emu_serial_history_destroy(callback_state.serial);
            return 2;
        }
        artifact_open = 1;
        callback_state.capture = &image_capture;
        callback_state.capture_open = want_lcd_capture;
        emu_artifact_request_t capture_request = {
            .action = EMU_ARTIFACT_START,
        };
        emu_artifact_result_t capture_result;
        int engine_artifacts =
            (engine->capabilities & EMU_CAP_ARTIFACT_REQ) != 0;
        if (options.want_lcd_frames && engine_artifacts) {
            capture_request.kind = EMU_ARTIFACT_LCD_FRAME;
            artifact_failed |= emu_session_artifact(
                session, &capture_request, &capture_result, &error) != EMU_OK;
        }
        if (options.want_lcd_ddram_frames && engine_artifacts) {
            capture_request.kind = EMU_ARTIFACT_LCD_DDRAM;
            artifact_failed |= emu_session_artifact(
                session, &capture_request, &capture_result, &error) != EMU_OK;
        }
        if (artifact_failed) {
            fprintf(stderr, "error: %s\n", error.message);
            (void)runtime_close_capture(
                session, &callback_state, &image_capture, &options, 1);
            emu_session_destroy(&session);
            emu_prepared_free(&prepared);
            emu_serial_history_destroy(callback_state.serial);
            return 2;
        }
        if (continuous && emu_session_poll(session, &error) != EMU_OK) {
            fprintf(stderr, "error: %s\n", error.message);
            (void)runtime_close_capture(
                session, &callback_state, &image_capture, &options, 1);
            emu_session_destroy(&session);
            emu_prepared_free(&prepared);
            emu_serial_history_destroy(callback_state.serial);
            return 2;
        }
    }
    char snapshot_provenance[2048] = "{}";
    if (options.want_snapshot && (continuous
            ? emu_prepared_rehash_source(&prepared, &error) != EMU_OK
            : runtime_snapshot_provenance(
            &prepared, options.firmware_patches,
            preparation.inherited_patches,
            preparation.effective_patches, snapshot_provenance,
            sizeof snapshot_provenance, &error) != 0)) {
        fprintf(stderr, "error: %s\n",
                error.message[0] ? error.message
                                 : "invalid snapshot provenance");
        artifact_failed |= runtime_close_capture(
            session, &callback_state, &image_capture, &options, 1);
        emu_session_destroy(&session);
        emu_prepared_free(&prepared);
        emu_serial_history_destroy(callback_state.serial);
        return 2;
    }
    const char *key_names[EMU_MAX_LOGICAL_KEYS] = {0};
    emu_ui_socket_t ui;
    emu_ui_session_binding_t ui_binding = {0};
    emu_ui_engine_t ui_engine = {0};
    int ui_open = 0;
    if (!product || !product->key_count || product->key_count > EMU_MAX_LOGICAL_KEYS) {
        fprintf(stderr, "error: input key catalog is unavailable\n");
        artifact_failed |= runtime_close_capture(
            session, &callback_state, &image_capture, &options, 1);
        emu_session_destroy(&session);
        emu_prepared_free(&prepared);
        emu_serial_history_destroy(callback_state.serial);
        return 2;
    }
    for (size_t i = 0; i < product->key_count; i++)
        key_names[i] = product->keys[i].name;
    ui_binding = (emu_ui_session_binding_t){
        .session = session, .keys = key_names, .key_count = product->key_count,
    };
    int input_bind_result = emu_ui_bind_session(&ui_engine, &ui_binding);
    if (options.ui_socket_path) {
        emu_engine_state_t ui_state = {0};
        int state_result = emu_session_query_request(
            session, 0, &ui_state, &error);
        int bind_result = input_bind_result;
        int open_result = bind_result == 0 && state_result == EMU_OK
            ? emu_ui_socket_open(
                &ui, options.ui_socket_path, prepared.selected_device,
                options.serial_pty_path == NULL,
                (engine->capabilities & EMU_CAP_AUDIO) != 0 &&
                    ui_state.audio_available,
                &ui_engine) : -1;
        if (bind_result != 0 || open_result != 0) {
            fprintf(stderr, "error: cannot listen on UI socket %s\n",
                    options.ui_socket_path);
            artifact_failed |= runtime_close_capture(
                session, &callback_state, &image_capture, &options, 1);
            emu_session_destroy(&session);
            emu_prepared_free(&prepared);
            emu_serial_history_destroy(callback_state.serial);
            return 2;
        }
        ui_open = 1;
        callback_state.ui = &ui;
        callback_state.ui_open = 1;
        if (!options.serial_pty_path) {
            emu_serial_link_state_t link_state;
            if (emu_session_serial_link(
                    session, EMU_SERIAL_LINK_UI,
                    &link_state, &error) != EMU_OK) {
                fprintf(stderr, "error: %s\n", error.message);
                callback_state.ui_open = 0;
                emu_ui_socket_close(&ui);
                artifact_failed |= runtime_close_capture(
                    session, &callback_state, &image_capture,
                    &options, 1);
                emu_session_destroy(&session);
                emu_prepared_free(&prepared);
                emu_serial_history_destroy(callback_state.serial);
                return 2;
            }
        }
        emu_ui_socket_bind_serial(&ui, callback_state.serial);
        if (emu_session_poll(session, &error) != EMU_OK ||
            callback_state.frame_failed) {
            fprintf(stderr, "error: UI socket poll failed\n");
            callback_state.ui_open = 0;
            emu_ui_socket_close(&ui);
            artifact_failed |= runtime_close_capture(
                session, &callback_state, &image_capture, &options, 1);
            emu_session_destroy(&session);
            emu_prepared_free(&prepared);
            emu_serial_history_destroy(callback_state.serial);
            return 2;
        }
    }

    emu_serial_pty_t serial_pty;
    emu_serial_session_binding_t serial_binding = {.session = session};
    emu_serial_engine_t serial_engine = {0};
    int serial_pty_open = 0;
    if (options.serial_pty_path) {
        char pty_error[768];
        if (emu_serial_bind_session(&serial_engine, &serial_binding) != 0 ||
            emu_serial_pty_open(
                &serial_pty, options.serial_pty_path, &serial_engine,
                pty_error, sizeof pty_error) != 0) {
            fprintf(stderr, "error: %s\n", pty_error);
            if (ui_open) {
                callback_state.ui_open = 0;
                emu_ui_socket_close(&ui);
            }
            artifact_failed |= runtime_close_capture(
                session, &callback_state, &image_capture, &options, 1);
            emu_session_destroy(&session);
            emu_prepared_free(&prepared);
            emu_serial_history_destroy(callback_state.serial);
            return 2;
        }
        serial_pty_open = 1;
        if (!options.benchmark_json)
            printf("serial-pty: %s -> %s\n", options.serial_pty_path,
                   serial_pty.slave_path);
    }
    if (!options.benchmark_json && !debug_mode)
        runtime_print_startup(&options, &prepared, &preparation, limit);

    emu_engine_state_t start_state = {0};
    if (emu_session_query_request(
            session, 0, &start_state, &error) != EMU_OK) {
        fprintf(stderr, "error: %s\n", error.message);
        if (serial_pty_open) emu_serial_pty_close(&serial_pty);
        if (ui_open) {
            callback_state.ui_open = 0;
            emu_ui_socket_close(&ui);
        }
        artifact_failed |= runtime_close_capture(
            session, &callback_state, &image_capture, &options, 1);
        emu_session_destroy(&session);
        emu_prepared_free(&prepared);
        emu_serial_history_destroy(callback_state.serial);
        return 2;
    }
    if (!managed_gdb) emu_runtime_reset_stop_signal();
    if (!managed_gdb && !emu_runtime_install_stop_handlers()) {
        fprintf(stderr, "error: cannot install signal handlers\n");
        if (serial_pty_open) emu_serial_pty_close(&serial_pty);
        if (ui_open) {
            callback_state.ui_open = 0;
            emu_ui_socket_close(&ui);
        }
        artifact_failed |= runtime_close_capture(
            session, &callback_state, &image_capture, &options, 1);
        emu_session_destroy(&session);
        emu_prepared_free(&prepared);
        emu_serial_history_destroy(callback_state.serial);
        return 2;
    }
    emu_trace_sink_t *trace_sink = NULL;
    emu_drcov_t *drcov = NULL;
    int trace_attached = 0;
    int drcov_attached = 0;
    int trace_failed = 0;
    int drcov_failed = 0;
    int trace_deferred = options.want_trace &&
        (options.trace_from_icount != 0 || options.trace_from_pc >= 0);
    int trace_armed = options.want_trace && !trace_deferred;
    int snapshot_done = 0;
    int snapshot_attempted = 0;
    int diagnostic_open_failed = 0;
    if (options.want_trace) {
        trace_sink = emu_trace_open(trace_path, options.trace_mask);
        if (!trace_sink) {
            fprintf(stderr, "error: cannot open trace %s\n", trace_path);
            diagnostic_open_failed = 1;
        }
    }
    if (!diagnostic_open_failed && options.want_drcov) {
        if (emu_session_drcov_attach(session, &drcov, &error) != EMU_OK) {
            fprintf(stderr, "error: cannot attach drcov collector\n");
            diagnostic_open_failed = 1;
        } else {
            drcov_attached = 1;
        }
    }
    if (!diagnostic_open_failed && trace_sink) {
        if (emu_session_trace_attach(
                session, trace_sink, trace_deferred,
                options.gsm_l1_trace_explicit, &error) != EMU_OK) {
            fprintf(stderr, "error: %s\n", error.message);
            diagnostic_open_failed = 1;
        } else {
            trace_attached = 1;
        }
    }
    if (diagnostic_open_failed) {
        if (trace_attached)
            (void)emu_session_trace_detach(session, &error);
        if (trace_sink) (void)emu_trace_close(trace_sink);
        if (drcov_attached)
            (void)emu_session_drcov_detach(session, &error);
        emu_drcov_destroy(&drcov);
        if (serial_pty_open) emu_serial_pty_close(&serial_pty);
        if (ui_open) {
            callback_state.ui_open = 0;
            emu_ui_socket_close(&ui);
        }
        artifact_failed |= runtime_close_capture(
            session, &callback_state, &image_capture, &options, 1);
        emu_session_destroy(&session);
        emu_prepared_free(&prepared);
        emu_serial_history_destroy(callback_state.serial);
        return 2;
    }
    int monitor_started = 0;
    if (options.monitor && !debug_mode) {
        if (emu_session_monitor_start(
                session, options.monitor_loop_threshold,
                options.monitor_stall_window, &error) != EMU_OK) {
            fprintf(stderr, "error: %s\n", error.message);
            if (trace_attached)
                (void)emu_session_trace_detach(session, &error);
            if (trace_sink) (void)emu_trace_close(trace_sink);
            if (drcov_attached)
                (void)emu_session_drcov_detach(session, &error);
            emu_drcov_destroy(&drcov);
            if (serial_pty_open) emu_serial_pty_close(&serial_pty);
            if (ui_open) {
                callback_state.ui_open = 0;
                emu_ui_socket_close(&ui);
            }
            artifact_failed |= runtime_close_capture(
                session, &callback_state, &image_capture, &options, 1);
            emu_session_destroy(&session);
            emu_prepared_free(&prepared);
            emu_serial_history_destroy(callback_state.serial);
            return 2;
        }
        monitor_started = 1;
    }
    const uint64_t execution_chunk_cap = 500000;

    int serial_printed = 0;
    emu_runtime_descriptor_t descriptor = {
        .width = product->display.width, .height = product->display.height,
        .key_count = product->key_count,
        .asc0_available = options.serial_pty_path == NULL,
        .audio_available = (engine->capabilities & EMU_CAP_AUDIO) != 0 &&
                           start_state.audio_available,
    };
    snprintf(descriptor.model, sizeof descriptor.model, "%s", prepared.selected_device);
    for (size_t i = 0; i < descriptor.key_count; i++)
        snprintf(descriptor.keys[i], sizeof descriptor.keys[i], "%s", product->keys[i].name);
    emu_runtime_state_t *runtime = emu_runtime_state_create(&descriptor, NULL);
    runtime_input_binding_t input_binding = {
        .callbacks = &callback_state,
        .session = session, .failed = &callback_state.diagnostic_failed,
        .engine = &ui_engine, .sink = &trace_sink, .armed = &trace_armed,
    };
    emu_input_callbacks_t input_callbacks = {
        .opaque = &input_binding, .set = runtime_input_set,
        .sampled = runtime_input_sampled, .trace = runtime_input_trace,
    };
    emu_input_t *input = runtime
        ? emu_input_create(key_names, product->key_count, &input_callbacks) : NULL;
    if (!input) {
        fprintf(stderr, "error: cannot initialize runtime/input services\n");
        if (ui_open) {
            callback_state.ui_open = 0;
            emu_ui_socket_close(&ui);
        }
        if (serial_pty_open) emu_serial_pty_close(&serial_pty);
        if (monitor_started) (void)emu_session_monitor_stop(session, &error);
        artifact_failed |= runtime_close_capture(
            session, &callback_state, &image_capture, &options, 1);
        if (trace_attached) (void)emu_session_trace_detach(session, &error);
        if (trace_sink) (void)emu_trace_close(trace_sink);
        if (drcov_attached) (void)emu_session_drcov_detach(session, &error);
        emu_drcov_destroy(&drcov);
        emu_runtime_state_destroy(runtime);
        emu_session_destroy(&session);
        emu_prepared_free(&prepared);
        emu_serial_history_destroy(callback_state.serial);
        return 2;
    }
    emu_serial_history_trace(callback_state.serial, runtime_serial_trace, &input_binding);
    if (ui_open) emu_ui_socket_bind_input(&ui, input);
    if (ui_open) emu_ui_socket_bind_runtime(&ui, runtime);
    struct timespec started, finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    uint64_t executed = 0, guest = 0;
    uint64_t current_icount = start_state.icount;
    uint32_t current_pc = start_state.pc;
    uint64_t next_ui_poll = current_icount;
    uint64_t next_serial_poll = current_icount;
    uint64_t next_input_poll = current_icount;
    const char *status = callback_state.serial_failed ? "serial_error" : "limit";
    char monitor_status[32] = "";
    char status_reason[704] = "";
    emu_run_result_t run = {0};
    uint64_t observation_ticks = 0, sample_allowance = 50000;
    uint64_t invocation_ns = (uint64_t)started.tv_sec * UINT64_C(1000000000) + started.tv_nsec;
    if (continuous && start_state.measured_ns) invocation_ns = start_state.measured_ns;
    uint64_t observation_ns = invocation_ns, publication_ns = invocation_ns;
    emu_runtime_snapshot_t snapshot = {
        .icount = current_icount, .pc = current_pc, .measured_ns = invocation_ns,
    };
    (void)emu_runtime_state_publish(runtime, &snapshot, 1);
    (void)emu_runtime_state_transition(runtime, EMU_RUNTIME_RUNNING, "", "");
    if (options.gdb_enabled && !managed_gdb) {
        printf("gdb: listening on 127.0.0.1:%u (firmware paused at %s)\n",
               options.gdb_port, options.from_snapshot ? "restored state" : "reset");
        printf("gdb: target remote 127.0.0.1:%u\n", options.gdb_port);
        fflush(stdout);
    }
    if (ticker) emu_runtime_ticker_refresh(stdout, &terminal, runtime, invocation_ns, 1);
    int continuous_complete = 0;
    if (continuous && emu_session_start(session, &error) != EMU_OK) {
        runtime_console_error(&callback_state, "error: %s\n", error.message);
        status = "error";
    }
    if (managed_gdb && !strcmp(status, "limit") && !emu_runtime_stop_signal() &&
        emu_gdb_start(gdb, &prepared.options)) status = "error";
    if (debug_mode && !managed_gdb) {
        char *script = options.dbg_script ? runtime_read_text_file(options.dbg_script) : NULL;
        runtime_debugger_t debugger = {
            .session = session, .runtime = runtime, .input = input,
            .callbacks = &callback_state, .ui = ui_open ? &ui : NULL,
            .pty = serial_pty_open ? &serial_pty : NULL,
            .interactive = options.dbg_interactive ||
                           (!options.dbg_command && !options.dbg_script),
            .invocation_ns = invocation_ns,
        };
        emu_debugger_request_t debugger_request = {
            .script = script, .command = options.dbg_command,
            .interactive = debugger.interactive,
            .monitor = options.monitor, .batch_idle = options.batch_idle,
            .input = {.opaque = stdin, .read = runtime_text_read},
            .output = {.opaque = stdout, .write = runtime_text_write},
            .input_wait = runtime_text_wait,
            .service = runtime_debugger_service, .service_opaque = &debugger,
        };
        int input_flags = fcntl(STDIN_FILENO, F_GETFL);
        int input_setup_failed = debugger.interactive &&
            (input_flags < 0 || fcntl(STDIN_FILENO, F_SETFL, input_flags | O_NONBLOCK) < 0);
        if (input_setup_failed) {
            runtime_console_error(&callback_state, "error: cannot configure debugger input\n");
            status = "error";
        } else if (options.dbg_script && !script) {
            runtime_console_error(&callback_state, "error: cannot read script %s\n", options.dbg_script);
            status = "error";
        } else if (emu_session_debugger_run(session, &debugger_request, &error) != EMU_OK) {
            runtime_console_error(&callback_state, "error: %s\n", error.message);
            status = "error";
        } else status = "stopped";
        if (debugger.interactive && input_flags >= 0 &&
            fcntl(STDIN_FILENO, F_SETFL, input_flags) < 0) status = "error";
        free(script);
        executed = debugger.ticks;
        guest = debugger.guest;
        current_icount = debugger.state.icount;
        current_pc = debugger.state.pc;
    }
    while (!strcmp(status, "limit") && (continuous || executed < limit)) {
        int stop = emu_runtime_stop_signal();
        if (managed_gdb && gdb->child && options.dbg_interactive && stop == SIGINT) {
            emu_runtime_reset_stop_signal();
            if (emu_gdb_interrupt(gdb)) { status = "error"; break; }
        } else if (stop) break;
        if (managed_gdb) {
            int ended = emu_gdb_poll(gdb);
            if (ended) { status = ended < 0 ? "error" : "stopped"; break; }
        }
        if (callback_state.diagnostic_failed) { status = "error"; break; }
        if (callback_state.serial_failed || (ui_open && ui.serial_failed)) {
            status = "serial_error";
            break;
        }
        emu_serial_history_context(callback_state.serial, current_icount, current_pc);
        if (serial_pty_open && (continuous || current_icount >= next_serial_poll)) {
            if (emu_serial_history_pty_poll(
                    callback_state.serial, callback_state.pty_subscription, &serial_pty) != 0) {
                runtime_console_error(&callback_state, "error: serial PTY poll failed: %s\n",
                        strerror(errno));
                status = "serial_error";
                break;
            }
            next_serial_poll = current_icount + 1000u;
        }
        if (continuous) {
            emu_engine_state_t observed;
            if (emu_session_query_request(session, 0, &observed, &error) != EMU_OK) {
                runtime_console_error(&callback_state, "error: %s\n", error.message);
                status = "error";
                break;
            }
            if (observed.measured_ns > observation_ns) {
                snapshot = (emu_runtime_snapshot_t){
                    .icount = observed.icount, .pc = observed.pc,
                    .ticks = observed.ticks, .guest_instructions = observed.guest_instructions,
                    .measured_ns = observed.measured_ns,
                    .elapsed_ns = observed.measured_ns - invocation_ns,
                };
                (void)emu_runtime_state_publish(runtime, &snapshot, 0);
                observation_ns = observed.measured_ns;
            }
            if (ticker) emu_runtime_ticker_refresh(
                stdout, &terminal, runtime, emu_runtime_monotonic_ns(), 0);
        } else if (executed != observation_ticks) {
            uint64_t now = emu_runtime_monotonic_ns();
            snapshot = (emu_runtime_snapshot_t){
                .icount = current_icount, .elapsed_ns = now - invocation_ns,
                .ticks = executed, .guest_instructions = guest,
                .pc = current_pc, .measured_ns = now,
            };
            if (emu_runtime_state_publish(runtime, &snapshot, 0) == 0) publication_ns = now;
            sample_allowance = emu_runtime_sample_allowance(
                executed - observation_ticks, now - observation_ns, now - publication_ns);
            observation_ticks = executed;
            observation_ns = now;
            if (ticker) emu_runtime_ticker_refresh(stdout, &terminal, runtime, now, 0);
        }
        emu_input_context(input, current_icount, current_pc);
        if ((ui_open || emu_input_pending(input)) &&
            (continuous || current_icount >= next_input_poll)) {
            if (emu_input_poll(input)) {
                runtime_console_error(&callback_state, "error: input release poll failed\n");
                status = "error";
                break;
            }
            next_input_poll = current_icount + EMU_INPUT_POLL_TICKS;
        }
        if (ui_open && (continuous || current_icount >= next_ui_poll)) {
            emu_ui_stats_t stats;
            emu_runtime_state_snapshot(runtime, &stats);
            if (emu_ui_socket_poll(&ui, &stats, 0) != 0) {
                runtime_console_error(&callback_state, "error: UI socket poll failed\n");
                status = ui.serial_failed ? "serial_error" : "ui_error";
                break;
            }
            next_ui_poll = current_icount + 50000u;
        }
        if (trace_sink && !trace_armed &&
            ((options.trace_from_icount &&
              current_icount >= options.trace_from_icount) ||
             (options.trace_from_pc >= 0 &&
              current_pc == (uint32_t)options.trace_from_pc))) {
            if (emu_session_trace_arm(session, &error) != EMU_OK) {
                runtime_console_error(&callback_state, "error: %s\n", error.message);
                status = "error";
                break;
            }
            trace_armed = 1;
        }
        if (continuous) {
            if (emu_session_pump(session, 10, &continuous_complete, &run, &error) != EMU_OK) {
                runtime_console_error(&callback_state, "error: %s\n", error.message);
                status = "error";
                break;
            }
            executed = run.ticks;
            guest = run.guest_instructions;
        } else {
            uint64_t remaining = limit - executed;
            uint64_t deadlines[] = {
                ui_open ? next_ui_poll : 0,
                emu_input_pending(input) ? next_input_poll : 0,
                serial_pty_open ? next_serial_poll : 0,
                trace_sink && !trace_armed ? options.trace_from_icount : 0,
                options.want_snapshot && !snapshot_done
                    ? options.snapshot_at : 0,
            };
            uint64_t allowance = emu_runtime_next_allowance(
                current_icount, remaining, deadlines,
                sizeof deadlines / sizeof deadlines[0]);
            if (allowance > sample_allowance) allowance = sample_allowance;
            if (allowance > execution_chunk_cap)
                allowance = execution_chunk_cap;
            if (trace_sink && !trace_armed &&
                options.trace_from_pc >= 0 && allowance > 1)
                allowance = 1;
            emu_run_request_t run_request = {
                .tick_budget = allowance,
                .allow_idle_batch = options.batch_idle &&
                    !monitor_started &&
                    !(trace_sink && !trace_armed &&
                      options.trace_from_pc >= 0),
            };
            if (emu_session_run_request(
                    session, &run_request, &run, &error) != EMU_OK) {
                runtime_console_error(&callback_state, "error: %s\n", error.message);
                status = "error";
                break;
            }
            executed += run.ticks;
            guest += run.guest_instructions;
        }
        current_icount = run.end_icount;
        current_pc = run.pc;
        emu_serial_history_context(callback_state.serial, current_icount, current_pc);
        if (callback_state.serial_failed) {
            runtime_console_error(&callback_state, "error: cannot buffer serial output\n");
            status = "serial_error";
            break;
        }
        if (callback_state.frame_failed) {
            runtime_console_error(&callback_state, "error: UI socket poll failed\n");
            status = "ui_error";
            break;
        }
        if (callback_state.capture_failed &&
            !callback_state.capture_error_reported) {
            runtime_console_error(&callback_state, "error: cannot write LCD capture frame\n");
            callback_state.capture_error_reported = 1;
            artifact_failed = 1;
        }
        if (!options.benchmark_json && !managed_gdb)
            runtime_flush_serial(&callback_state, &serial_printed, 0, ticker ? &terminal : NULL);
        if (options.want_snapshot && !snapshot_done &&
            options.snapshot_at &&
            current_icount == options.snapshot_at) {
            if (emu_session_snapshot_write(
                    session, snapshot_dir, snapshot_provenance,
                    options.want_snapshot_full_bins, &error) == EMU_OK)
                snapshot_done = 1;
            else
                artifact_failed = 1;
        }
        if (run.status == EMU_RUN_UNIMPLEMENTED) {
            status = emu_run_status_name(run.status);
            runtime_console_error(&callback_state, "unimplemented opcode %#x @ %#x\n",
                    run.unimplemented_opcode, run.unimplemented_pc);
            break;
        }
        if (monitor_started) {
            emu_monitor_verdict_t verdict;
            if (emu_session_monitor_poll(
                    session, &verdict, &error) != EMU_OK) {
                runtime_console_error(&callback_state, "error: %s\n", error.message);
                status = "error";
                break;
            }
            if (verdict.status[0]) {
                snprintf(monitor_status, sizeof monitor_status, "%s",
                         verdict.status);
                status = monitor_status;
                snprintf(status_reason, sizeof status_reason, "%s",
                         verdict.reason);
                break;
            }
        }
        if (continuous_complete) {
            status = managed_gdb ? "error" : "stopped";
            break;
        }
        if (!continuous && run.status != EMU_RUN_LIMIT) {
            status = emu_run_status_name(run.status);
            break;
        }
    }
    int signal_number = emu_runtime_stop_signal();
    if (managed_gdb && emu_gdb_stop(gdb)) status = "error";
    /* Preserve electrical cleanup and its trace before final artifact closure.
     * Keep the transport open only to drain the final runtime state afterward. */
    emu_input_context(input, current_icount, current_pc);
    if (ui_open) {
        emu_ui_socket_stop_input(&ui);
        if (ui.input_failed) artifact_failed = 1;
    }
    if (emu_input_destroy(input)) {
        runtime_console_error(&callback_state, "error: input teardown failed\n");
        artifact_failed = 1;
    }
    input = NULL;
    if (ui_open) emu_ui_socket_bind_input(&ui, NULL);
    if (callback_state.diagnostic_failed) status = "error";
    if (continuous && options.want_snapshot) {
        snapshot_attempted = 1;
        if (emu_session_snapshot_write(session, snapshot_dir,
                snapshot_provenance, 0, &error) == EMU_OK) {
            snapshot_done = 1;
        } else {
            runtime_console_error(&callback_state, "error: snapshot: %s\n", error.message);
            artifact_failed = 1;
        }
    }
    if (continuous && !continuous_complete) {
        if (emu_session_request_stop(session, &error) != EMU_OK) {
            runtime_console_error(&callback_state, "error: %s\n", error.message);
            status = "error";
        } else {
            for (unsigned i = 0; i < 500u && !continuous_complete; i++) {
                if (emu_session_pump(session, 10, &continuous_complete,
                                     &run, &error) != EMU_OK) {
                    runtime_console_error(&callback_state, "error: %s\n", error.message);
                    status = "error";
                    break;
                }
            }
            executed = run.ticks;
            guest = run.guest_instructions;
            current_icount = run.end_icount;
            current_pc = run.pc;
        }
    }
    if (ticker) emu_runtime_ticker_finish(stdout, &terminal);
    callback_state.ticker = NULL;
    if (serial_pty_open) {
        if (emu_serial_history_pty_poll(
                callback_state.serial, callback_state.pty_subscription, &serial_pty) != 0 &&
            strcmp(status, "error")) {
            runtime_console_error(&callback_state, "error: serial PTY poll failed: %s\n",
                    strerror(errno));
            status = "serial_error";
        }
        emu_serial_pty_close(&serial_pty);
    }
    if (want_lcd_capture) {
        emu_artifact_request_t final_capture = {
            .kind = options.want_lcd_frames ? EMU_ARTIFACT_LCD_FRAME
                                            : EMU_ARTIFACT_LCD_DDRAM,
            .action = EMU_ARTIFACT_CAPTURE,
        };
        emu_artifact_result_t capture_result;
        if ((engine->capabilities & EMU_CAP_ARTIFACT_REQ) &&
            emu_session_artifact(
                session, &final_capture, &capture_result, &error) != EMU_OK) {
            runtime_console_error(&callback_state, "error: cannot write LCD capture frame\n");
            artifact_failed = 1;
        }
        artifact_failed |= runtime_close_capture(
            session, &callback_state, &image_capture, &options, 1);
    }
    uint64_t trace_count = trace_sink ? trace_sink->count : 0;
    if (trace_attached &&
        emu_session_trace_detach(session, &error) != EMU_OK) {
        runtime_console_error(&callback_state, "error: %s\n", error.message);
        trace_failed = 1;
    }
    if (trace_sink && emu_trace_close(trace_sink) != 0) {
        runtime_console_error(&callback_state, "error: cannot finalize trace %s\n", trace_path);
        trace_failed = 1;
    }
    trace_sink = NULL;
    emu_drcov_statistics_t drcov_statistics = {0};
    if (drcov) {
        if (drcov_attached &&
            emu_session_drcov_detach(session, &error) != EMU_OK)
            drcov_failed = 1;
        if (!drcov_failed &&
            emu_drcov_write(
                drcov, drcov_path, &drcov_statistics) != EMU_DRCOV_OK)
            drcov_failed = 1;
        if (drcov_failed) {
            runtime_console_error(&callback_state, "error: cannot write drcov %s\n", drcov_path);
        } else if (!options.benchmark_json) {
            printf("drcov: %llu exec events -> %zu basic blocks, %d modules\n",
                   (unsigned long long)drcov_statistics.execution_events,
                   drcov_statistics.unique_blocks,
                   (int)drcov_statistics.covered_modules);
        }
        emu_drcov_destroy(&drcov);
    }
    artifact_failed |= trace_failed || drcov_failed || callback_state.diagnostic_failed;
    emu_engine_state_t state = {0};
    if (emu_session_query_request(session, 0, &state, &error) != EMU_OK) {
        runtime_console_error(&callback_state, "error: %s\n", error.message);
        status = "error";
    }
    status = emu_runtime_classify_status(
        status, signal_number, state.halted,
        state.idle && !state.idle_wake_possible);
    if (!options.benchmark_json && !debug_mode)
        runtime_flush_serial(&callback_state, &serial_printed, 1, ticker ? &terminal : NULL);
    clock_gettime(CLOCK_MONOTONIC, &finished);
    double elapsed = emu_runtime_elapsed_ns(&started, &finished) / 1e9;
    snapshot = (emu_runtime_snapshot_t){
        .icount = state.icount, .elapsed_ns = emu_runtime_elapsed_ns(&started, &finished),
        .ticks = executed, .guest_instructions = guest, .pc = state.pc,
        .measured_ns = continuous ? state.measured_ns :
            (uint64_t)finished.tv_sec * UINT64_C(1000000000) + finished.tv_nsec,
    };
    if (continuous) snapshot.elapsed_ns = snapshot.measured_ns - invocation_ns;
    (void)emu_runtime_state_publish(runtime, &snapshot, 1);
    (void)emu_runtime_state_transition(runtime, EMU_RUNTIME_STOPPED, status, status_reason);
    if (ui_open) {
        emu_runtime_state_snapshot(runtime, &snapshot);
        emu_ui_socket_drain(&ui, &snapshot);
        callback_state.ui_open = 0;
        emu_ui_socket_close(&ui);
    }
    emu_runtime_state_destroy(runtime);
    if (options.benchmark_json) {
        emu_engine_state_t digest_state = {0};
        if (emu_session_query_request(
                session, 1, &digest_state, &error) != EMU_OK) {
            fprintf(stderr, "error: %s\n", error.message);
            status = "error";
        } else {
            state.digest = digest_state.digest;
        }
        emu_benchmark_record_t benchmark = {
            .status = status,
            .reason = status_reason,
            .device = prepared.selected_device,
            .device_source = options.device_name ? "override" : "auto",
            .image = &prepared.metadata,
            .source_size = prepared.source.size,
            .flash_primary_offset = prepared.chips[0].source_offset,
            .flash_secondary_offset = prepared.chip_count > 1
                                    ? prepared.chips[1].source_offset : 0,
            .firmware_patches = preparation.effective_patches,
            .start_icount = start_state.icount,
            .end_icount = state.icount,
            .ticks = executed,
            .guest_instructions = guest,
            .elapsed_seconds = elapsed,
            .pc = state.pc,
            .state_digest = state.digest,
            .interrupt_cache_queries = state.interrupt_cache_queries,
            .interrupt_cache_hits = state.interrupt_cache_hits,
            .interrupt_cache_scans = state.interrupt_cache_scans,
            .interrupt_cache_invalidations =
                state.interrupt_cache_invalidations,
        };
        emu_benchmark_print(stdout, &benchmark);
    } else if (options.summary) {
        static char summary[1u << 18];
        emu_summary_request_t summary_request = {
            .status = status,
            .reason = status_reason,
            .steps = state.icount,
            .pc = state.pc,
            .elapsed_seconds = elapsed,
            .serial_already_printed = serial_printed,
            .color = isatty(STDOUT_FILENO),
            .show_writes = options.show_writes,
        };
        if (emu_session_summary_format(
                session, &summary_request, summary, sizeof summary,
                &error) != EMU_OK) {
            fprintf(stderr, "error: %s\n", error.message);
            status = "error";
        } else {
            fputs(summary, stdout);
        }
    } else if (!debug_mode) {
        double tick_rate = elapsed > 0 ? executed / elapsed : 0;
        double guest_rate = elapsed > 0 ? guest / elapsed : 0;
        printf("status: %s  ticks: %llu  icount: %llu  pc: %#08x\n",
               status, (unsigned long long)executed,
               (unsigned long long)state.icount, state.pc);
        if (status_reason[0])
            printf("reason: %s\n", status_reason);
        printf("time: %.3fs  (%.0f ticks/s, %.0f guest instr/s)\n",
               elapsed, tick_rate, guest_rate);
        printf("traps: %llu  interrupts: %llu  xbus-unknown-1(id/status/mbox): %llu/%llu/%llu\n",
               (unsigned long long)state.traps_taken,
               (unsigned long long)state.interrupts_delivered,
               (unsigned long long)state.xbus_unknown1_id_reads,
               (unsigned long long)state.xbus_unknown1_status_reads,
               (unsigned long long)state.xbus_unknown1_doorbell_rings);
    }
    int flash_written = 0;
    if (options.want_dump_flash) {
        if (runtime_write_flash(session, flash_path, &error) != 0) {
            fprintf(stderr, "error: cannot write flash image %s\n",
                    flash_path);
            artifact_failed = 1;
        } else {
            flash_written = 1;
        }
    }
    if (options.want_snapshot && !snapshot_done && !snapshot_attempted) {
        if (options.snapshot_at && state.icount < options.snapshot_at) {
            fprintf(stderr,
                    "WARNING: run stopped at icount=%llu before --snapshot-at=%llu; no snapshot written\n",
                    (unsigned long long)state.icount,
                    (unsigned long long)options.snapshot_at);
        } else if (emu_session_snapshot_write(
                       session, snapshot_dir, snapshot_provenance,
                       options.want_snapshot_full_bins,
                       &error) == EMU_OK) {
            snapshot_done = 1;
        } else {
            artifact_failed = 1;
        }
    }
    if (artifact_open && continuous) {
        emu_run_result_t manifest_result = {
            .status = !strcmp(status, "halted") ? EMU_RUN_HALTED :
                      !strcmp(status, "unimplemented")
                          ? EMU_RUN_UNIMPLEMENTED :
                      !strcmp(status, "limit") ? EMU_RUN_LIMIT
                                                : EMU_RUN_STOPPED,
            .ticks = executed,
            .guest_instructions = guest,
            .end_icount = state.icount,
            .pc = state.pc,
            .digest = state.digest,
        };
        if (runtime_write_manifest(
                &artifact_run, &prepared, engine, &request, &preparation,
                &manifest_result, options.want_dump_flash,
                &error) != 0) {
            fprintf(stderr, "error: cannot write session manifest: %s\n",
                    error.message[0] ? error.message : "artifact I/O failed");
            artifact_failed = 1;
        }
    }
    if (artifact_open &&
        emu_artifact_run_finalize(&artifact_run, state.icount) != 0) {
        fprintf(stderr, "error: cannot finalize artifact run %s\n",
                artifact_run.path);
        artifact_failed = 1;
    }
    if (artifact_open && !options.benchmark_json) {
        printf("output -> %s/\n", artifact_run.path);
        if (debug_mode && options.want_trace)
            printf("trace: %llu events\n", (unsigned long long)trace_count);
        if (options.want_dump_flash && flash_written && !artifact_failed)
            printf("flash image: %s/flash.bin\n", artifact_run.path);
        if (options.want_lcd_frames) {
            printf("lcd frames: %llu\n",
                   (unsigned long long)image_capture.strict_written);
            printf("lcd animation: %s/lcd/frames.gif\n",
                   artifact_run.path);
        }
        if (options.want_lcd_ddram_frames)
            printf("lcd DDRAM frames: %llu\n",
                   (unsigned long long)image_capture.raw_written);
    }
    int failed = !strcmp(status, "error") ||
                 !strcmp(status, "serial_error") ||
                 !strcmp(status, "ui_error") || artifact_failed;
    if (monitor_started &&
        emu_session_monitor_stop(session, &error) != EMU_OK) {
        fprintf(stderr, "error: %s\n", error.message);
        failed = 1;
    }
    emu_session_destroy(&session);
    emu_prepared_free(&prepared);
    emu_serial_history_destroy(callback_state.serial);
    return failed ? 2 : signal_number ? emu_runtime_exit_code(0, signal_number)
        : managed_gdb ? gdb->exit_code : 0;
}

int emu_cemu_runtime_main(
        int argc, char **argv, const emu_engine_descriptor_t *engine) {
    return emu_runtime_main(argc, argv, engine, NULL, NULL);
}

int emu_qemu_runtime_main(
        int argc, char **argv, const emu_engine_descriptor_t *engine,
        const char *default_binary) {
    emu_gdb_t gdb = {0};
    int rc = emu_runtime_main(argc, argv, engine, default_binary, &gdb);
    return emu_gdb_cleanup(&gdb) ? 2 : rc;
}
