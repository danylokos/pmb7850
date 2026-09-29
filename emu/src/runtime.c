#define _POSIX_C_SOURCE 200809L

#include "emu_runtime.h"

#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

static volatile sig_atomic_t emu_stop_signal;

static void emu_request_stop(int signal_number) {
    if (!emu_stop_signal || signal_number == SIGTERM) emu_stop_signal = signal_number;
}

int emu_runtime_install_stop_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = emu_request_stop;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGINT, &action, NULL) == 0 &&
           sigaction(SIGTERM, &action, NULL) == 0;
}

void emu_runtime_reset_stop_signal(void) { emu_stop_signal = 0; }
int emu_runtime_stop_signal(void) { return emu_stop_signal; }

uint64_t emu_runtime_elapsed_ns(const struct timespec *start,
                                const struct timespec *end) {
    int64_t seconds = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
    int64_t nanoseconds = (int64_t)end->tv_nsec - (int64_t)start->tv_nsec;
    return (uint64_t)(seconds * INT64_C(1000000000) + nanoseconds);
}

int emu_runtime_ticker_enabled(int benchmark_json, int stdout_is_tty) {
    return !benchmark_json && stdout_is_tty;
}

uint64_t emu_runtime_next_allowance(uint64_t current, uint64_t remaining,
                                    const uint64_t *deadlines, size_t count) {
    uint64_t allowance = remaining;
    for (size_t i = 0; i < count; i++) {
        if (deadlines[i] > current && deadlines[i] - current < allowance)
            allowance = deadlines[i] - current;
    }
    return allowance;
}

uint64_t emu_runtime_sample_allowance(uint64_t ticks, uint64_t interval_ns,
                                      uint64_t since_publication_ns) {
    if (!interval_ns) return 50000;
    uint64_t remaining = since_publication_ns < UINT64_C(10000000)
                       ? UINT64_C(10000000) - since_publication_ns : 1;
    double estimate = (double)ticks * (double)remaining / (double)interval_ns;
    if (estimate < 1) return 1;
    if (estimate > 500000) return 500000;
    return (uint64_t)estimate;
}

/* Divide before rounding so UINT64_MAX counters never overflow or lose bits. */
static void ticker_fixed(FILE *out, uint64_t value, uint64_t quantum,
                         uint64_t scale, int digits) {
    uint64_t rounded = value / quantum + (value % quantum >= quantum / 2);
    fprintf(out, "%" PRIu64 ".%0*" PRIu64, rounded / scale, digits, rounded % scale);
}

static void ticker_rate(FILE *out, double rate) {
    double millions = rate / 1e6;
    uint64_t whole = (uint64_t)millions;
    /* JS toFixed chooses the larger decimal at exact ties. Of the halfway
     * fractions at one decimal place, only .25 and .75 are binary-exact.
     * printf's ties-to-even already rounds .75 upward. */
    if (millions - (double)whole == 0.25)
        fprintf(out, "%" PRIu64 ".3", whole);
    else
        fprintf(out, "%.1f", millions);
}

static void ticker_format(FILE *out, const emu_runtime_snapshot_t *snapshot,
                             const emu_runtime_lifecycle_t *lifecycle) {
    const char *status = lifecycle->phase == EMU_RUNTIME_RUNNING ? "running" :
                         lifecycle->phase == EMU_RUNTIME_STOPPED ? lifecycle->status : "initialized";
    fprintf(out, "  %s: ", status);
    ticker_fixed(out, snapshot->elapsed_ns, 10000000, 100, 2);
    fputs("s  ", out);
    ticker_fixed(out, snapshot->ticks, 100000, 10, 1);
    fputs("M ticks  ", out);
    if (snapshot->rates_valid) {
        ticker_rate(out, snapshot->ticks_per_s);
        fputs("M ticks/s  ", out);
        ticker_rate(out, snapshot->guest_instructions_per_s);
        fputs("M guest instr/s", out);
    } else
        fputs("-- ticks/s  -- guest instr/s", out);
    fprintf(out, "  pc 0x%06" PRIx32, snapshot->pc);
    fflush(out);
}

void emu_runtime_draw_ticker(FILE *out, const emu_runtime_snapshot_t *snapshot,
                             const emu_runtime_lifecycle_t *lifecycle) {
    fputs("\r\033[K", out);
    ticker_format(out, snapshot, lifecycle);
}

int emu_runtime_ticker_refresh(FILE *out, emu_runtime_ticker_t *ticker,
                               const emu_runtime_state_t *runtime, uint64_t now_ns,
                               int force) {
    if (!force && ticker->has_displayed &&
        now_ns - ticker->displayed_ns < ticker->interval_ns) return 0;
    emu_runtime_state_snapshot(runtime, &ticker->snapshot);
    emu_runtime_state_lifecycle(runtime, &ticker->lifecycle);
    ticker->has_displayed = 1;
    ticker->displayed_ns = now_ns;
    emu_runtime_ticker_restore(out, ticker);
    return 1;
}

static unsigned terminal_columns(FILE *out) {
    struct winsize size;
    return ioctl(fileno(out), TIOCGWINSZ, &size) == 0 && size.ws_col
        ? size.ws_col : 80;
}

void emu_runtime_ticker_clear(FILE *out, emu_runtime_ticker_t *ticker) {
    /* The cursor rests at the start of the live block. Erasing downward also
     * removes rows reflowed by a terminal resize, without guessing their count. */
    if (ticker->visible_rows) fputs("\r\033[J", out);
    ticker->visible_rows = 0;
    fflush(out);
}

static void ticker_row(FILE *out, const char *text, unsigned columns) {
    size_t size = strlen(text);
    if (size >= columns) size = columns - 1;
    fwrite(text, 1, size, out);
}

void emu_runtime_ticker_restore(FILE *out, emu_runtime_ticker_t *ticker) {
    if (!ticker->has_displayed || ticker->console_partial) return;
    char *text = NULL;
    size_t size = 0;
    FILE *buffer = open_memstream(&text, &size);
    if (!buffer) return;
    ticker_format(buffer, &ticker->snapshot, &ticker->lifecycle);
    fclose(buffer);
    int visible = ticker->visible_rows != 0;
    emu_runtime_ticker_clear(out, ticker);
    unsigned columns = terminal_columns(out);
    if (!visible) fputs("\r\033[K", out);
    if (ticker->warning[0]) {
        ticker_row(out, ticker->warning, columns);
        ticker->visible_rows = 1;
        fputs("\r\n\033[K", out);
    }
    ticker_row(out, text, columns);
    ticker->visible_rows++;
    /* Park at column zero of the first row so resizing or echoed ^C cannot
     * leave the cursor below a wrapped status row. All live text is below us. */
    fputc('\r', out);
    if (ticker->visible_rows == 2) fputs("\033[A", out);
    free(text);
    fflush(out);
}

void emu_runtime_ticker_console(FILE *out, emu_runtime_ticker_t *ticker,
                                emu_console_kind_t kind,
                                const char *text, size_t size) {
    if (ticker && kind == EMU_CONSOLE_DELAY_WARNING) {
        while (size && (text[size - 1] == '\n' || text[size - 1] == '\r')) size--;
        if (size >= sizeof ticker->warning) size = sizeof ticker->warning - 1;
        memcpy(ticker->warning, text, size);
        ticker->warning[size] = 0;
    } else {
        if (ticker) emu_runtime_ticker_clear(out, ticker);
        fwrite(text, 1, size, out);
        if (kind == EMU_CONSOLE_DELAY_WARNING && size && text[size - 1] != '\n')
            fputc('\n', out);
        if (ticker && size) ticker->console_partial = text[size - 1] != '\n';
    }
    if (ticker) emu_runtime_ticker_restore(out, ticker);
    fflush(out);
}

void emu_runtime_ticker_finish(FILE *out, emu_runtime_ticker_t *ticker) {
    emu_runtime_ticker_clear(out, ticker);
    if (ticker->console_partial) fputc('\n', out);
    ticker->console_partial = 0;
    ticker->has_displayed = 0;
    ticker->warning[0] = 0;
    fflush(out);
}

const char *emu_runtime_classify_status(const char *engine_status,
                                        int signal_number,
                                        int halted, int idle) {
    if (signal_number) return "interrupted";
    if (engine_status && strcmp(engine_status, "limit")) return engine_status;
    if (halted) return "halted";
    if (idle) return "idle";
    return engine_status ? engine_status : "limit";
}

int emu_runtime_exit_code(int artifact_failed, int signal_number) {
    if (artifact_failed) return 2;
    return signal_number ? 128 + signal_number : 0;
}

static void emu_json_string(FILE *out, const char *text) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)(text ? text : "");
         *p; p++) {
        switch (*p) {
            case '"': fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\b': fputs("\\b", out); break;
            case '\f': fputs("\\f", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                if (*p < 0x20u) fprintf(out, "\\u%04x", *p);
                else fputc(*p, out);
        }
    }
    fputc('"', out);
}

void emu_benchmark_print(FILE *out, const emu_benchmark_record_t *record) {
    const emu_image_metadata_t *image = record->image;
    double tick_rate = record->elapsed_seconds > 0
                     ? record->ticks / record->elapsed_seconds : 0;
    double guest_rate = record->elapsed_seconds > 0
                      ? record->guest_instructions / record->elapsed_seconds : 0;
    fprintf(out, "{\"schema\":2,\"status\":");
    emu_json_string(out, record->status);
    fputs(",\"reason\":", out); emu_json_string(out, record->reason);
    fputs(",\"device\":", out); emu_json_string(out, record->device);
    fputs(",\"device_source\":", out);
    emu_json_string(out, record->device_source);
    fputs(",\"image_model\":", out); emu_json_string(out, image->model);
    fprintf(out, ",\"image_software\":%d,\"image_langpack\":",
            image->software_version);
    emu_json_string(out, image->langpack);
    fputs(",\"image_bcore_software\":", out);
    if (image->bcore_software_version >= 0)
        fprintf(out, "%d", image->bcore_software_version);
    else fputs("null", out);
    fprintf(out, ",\"image_bcore_software_raw\":%u,"
                 "\"image_metadata_view_offset\":%zu,"
                 "\"image_metadata_instances\":%zu,"
                 "\"flash_file_order\":",
            image->bcore_software_version_raw, image->metadata_view_offset,
            image->metadata_instance_count);
    emu_json_string(out, image->flash_file_order);
    fprintf(out, ",\"flash_primary_offset\":%zu,"
                 "\"flash_secondary_offset\":",
            record->flash_primary_offset);
    if (!strcmp(image->flash_file_order, "single")) fputs("null", out);
    else fprintf(out, "%zu", record->flash_secondary_offset);
    fputs(",\"flash_vendor\":", out); emu_json_string(out, image->flash_vendor);
    fprintf(out, ",\"flash_manufacturer_id\":%u,\"flash_device_id\":%u,"
                 "\"flash_engine\":",
            image->flash_manufacturer_id, image->flash_device_id);
    emu_json_string(out, image->flash_engine);
    fputs(",\"flash_engine_classification\":", out);
    emu_json_string(out, image->flash_classification);
    fputs(",\"firmware_patches\":", out);
    emu_patch_print_json(out, record->firmware_patches);
    fprintf(out, ",\"start_icount\":%llu,\"end_icount\":%llu,"
                 "\"ticks\":%llu,\"guest_instructions\":%llu,"
                 "\"elapsed_s\":%.9f,\"ticks_per_s\":%.6f,"
                 "\"guest_instructions_per_s\":%.6f,\"pc\":%u,"
                 "\"state_digest\":\"%016llx\","
                 "\"interrupt_cache_queries\":%llu,"
                 "\"interrupt_cache_hits\":%llu,"
                 "\"interrupt_cache_scans\":%llu,"
                 "\"interrupt_cache_invalidations\":%llu}\n",
            (unsigned long long)record->start_icount,
            (unsigned long long)record->end_icount,
            (unsigned long long)record->ticks,
            (unsigned long long)record->guest_instructions,
            record->elapsed_seconds, tick_rate, guest_rate, record->pc,
            (unsigned long long)record->state_digest,
            (unsigned long long)record->interrupt_cache_queries,
            (unsigned long long)record->interrupt_cache_hits,
            (unsigned long long)record->interrupt_cache_scans,
            (unsigned long long)record->interrupt_cache_invalidations);
}
