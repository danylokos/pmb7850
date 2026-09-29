/* PCF8813 atomic repaint publication tests. */
#include <stdio.h>
#include <string.h>

#include "pcf8813.h"
#include "soc.h"

static int g_fail, g_total_fail, g_total_run;
#define CHECK(cond) do { if (!(cond)) { g_fail++; \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define P3 0xFFC4u

typedef struct {
    unsigned frames, transactions;
    uint32_t last_frame_bytes, last_transaction_bytes;
    uint64_t last_sequence;
    unsigned last_x, last_y;
    char last_disposition[16];
    char last_boundary[24];
} trace_capture_t;

static long field_i64(const cemu_native_trace_event_t *trace,
                      const char *key) {
    for (int i = 0; i < trace->info.n; i++)
        if (!strcmp(trace->info.kv[i].key, key))
            return trace->info.kv[i].ival;
    return -1;
}

static const char *field_string(const cemu_native_trace_event_t *trace,
                                const char *key) {
    for (int i = 0; i < trace->info.n; i++)
        if (!strcmp(trace->info.kv[i].key, key))
            return trace->info.kv[i].sval;
    return NULL;
}

static int trace_filter(void *opaque, const char *kind) {
    (void)opaque;
    return !strcmp(kind, "lcd_frame") || !strcmp(kind, "lcd_transaction");
}

static void trace_consumer(void *opaque, const cemu_event_t *event) {
    trace_capture_t *capture = opaque;
    const cemu_native_trace_event_t *trace = event->as.peripheral.trace;
    if (!strcmp(trace->kind, "lcd_frame")) {
        capture->frames++;
        capture->last_frame_bytes = (uint32_t)field_i64(trace, "data_bytes");
        const char *boundary = field_string(trace, "boundary_reason");
        snprintf(capture->last_boundary, sizeof capture->last_boundary,
                 "%s", boundary ? boundary : "");
    } else if (!strcmp(trace->kind, "lcd_transaction")) {
        capture->transactions++;
        capture->last_sequence = (uint64_t)field_i64(trace, "sequence");
        capture->last_transaction_bytes =
            (uint32_t)field_i64(trace, "data_bytes");
        capture->last_x = (unsigned)field_i64(trace, "x");
        capture->last_y = (unsigned)field_i64(trace, "y");
        const char *disposition = field_string(trace, "disposition");
        snprintf(capture->last_disposition,
                 sizeof capture->last_disposition, "%s",
                 disposition ? disposition : "");
    }
}

typedef struct {
    soc_t soc;
    peripheral_t periph;
    pcf8813_state_t state;
    trace_capture_t trace;
} fixture_t;

static void set_select(fixture_t *f, int selected) {
    uint16_t value = memory_controller_sfr_get(&f->soc.memory, P3);
    if (selected) value &= (uint16_t)~(1u << 11);
    else value |= (uint16_t)(1u << 11);
    cemu_memory_controller_sfr_put(&f->soc.memory, P3, value);
    f->periph.tick(&f->periph, &f->soc, 1);
}

static void setup(fixture_t *f) {
    memset(f, 0, sizeof *f);
    device_lcd_config_t cfg = {
        .model = "pcf8813",
        .select_addr = P3,
        .select_bit = 11,
        .select_active_low = 1,
        .dc_addr = 0xFFD0,
        .dc_bit = 6,
        .reset_capcom_channel = -1,
        .panel_width = PCF8813_WIDTH,
        .panel_height = 64,
    };
    cemu_event_hub_init(&f->soc.instrumentation);
    cemu_event_subscribe_filtered(&f->soc.instrumentation,
                                  CEMU_EVENT_PERIPHERAL,
                                  trace_consumer, trace_filter, &f->trace);
    cemu_pcf8813_periph_init(&f->periph, &f->state, &cfg);
    set_select(f, 0);
    f->state.power_down = 0;
    f->state.power_control = 1;
    f->state.display_mode = PCF8813_DISPLAY_NORMAL;
}

static void write_bank(fixture_t *f, unsigned bank, unsigned x,
                       unsigned bytes, uint8_t value) {
    f->state.x = (uint8_t)x;
    f->state.y = (uint8_t)bank;
    set_select(f, 1);
    for (unsigned i = 0; i < bytes; i++)
        cemu_pcf8813_write_byte(&f->periph, &f->soc, 1,
                                (uint8_t)(value + i));
    set_select(f, 0);
}

static void test_full_sweep_and_render_hold(void) {
    fixture_t f;
    setup(&f);
    write_bank(&f, 0, 0, 102, 1);
    CHECK(f.state.ddram[0] == 1);
    CHECK(f.state.presented_ddram[0] == 0);
    CHECK(f.state.common.data_seq == 102);
    CHECK(f.state.common.frame_seq == 0 && f.trace.frames == 0);
    CHECK(f.state.sweep.active && f.state.sweep.expected_y == 1);
    CHECK(!strcmp(f.trace.last_disposition, "held"));

    uint8_t visible[PCF8813_WIDTH * PCF8813_HEIGHT * 3u];
    uint8_t raw[sizeof visible];
    CHECK(cemu_pcf8813_render_rgb(&f.state, 1, visible,
                                  sizeof visible) == 0);
    CHECK(cemu_pcf8813_render_ddram_rgb(&f.state, 1, raw,
                                        sizeof raw) == 0);
    CHECK(visible[0] == 202 && visible[1] == 214 && visible[2] == 184);
    CHECK(raw[0] == 30 && raw[1] == 39 && raw[2] == 32);

    for (unsigned bank = 1; bank < 8; bank++)
        write_bank(&f, bank, 0, 102, (uint8_t)(bank + 1));
    CHECK(f.state.common.frame_seq == 1 && f.trace.frames == 1);
    CHECK(f.trace.last_frame_bytes == 816);
    CHECK(f.state.transaction_count == 8 && f.trace.transactions == 8);
    CHECK(f.trace.last_sequence == 8 && f.trace.last_transaction_bytes == 102);
    CHECK(f.trace.last_x == 0 && f.trace.last_y == 7);
    CHECK(!strcmp(f.trace.last_disposition, "committed"));
    CHECK(!strcmp(f.trace.last_boundary, "viewport_complete"));
    CHECK(!f.state.sweep.active && f.state.sweep.data_bytes == 0);
    CHECK(!memcmp(f.state.ddram, f.state.presented_ddram,
                  sizeof f.state.ddram));
}

static void test_early_coordinate_mode_and_restart(void) {
    fixture_t f;
    setup(&f);
    write_bank(&f, 0, 0, 102, 0x11);
    f.state.x = 1;
    f.state.y = 1;
    set_select(&f, 1);
    cemu_pcf8813_write_byte(&f.periph, &f.soc, 1, 0x77);
    CHECK(f.state.common.frame_seq == 1 && f.trace.last_frame_bytes == 102);
    CHECK(f.state.presented_ddram[0] == 0x11);
    CHECK(f.state.presented_ddram[PCF8813_WIDTH + 1] == 0);
    set_select(&f, 0);
    CHECK(f.state.common.frame_seq == 1);
    CHECK(!strcmp(f.trace.last_disposition, "committed"));

    setup(&f);
    write_bank(&f, 0, 0, 102, 0x21);
    f.state.x = 0;
    f.state.y = 1;
    f.state.vertical = 1;
    set_select(&f, 1);
    cemu_pcf8813_write_byte(&f.periph, &f.soc, 1, 0x31);
    CHECK(f.state.common.frame_seq == 1 && f.state.sweep.active);
    set_select(&f, 0);

    setup(&f);
    write_bank(&f, 0, 0, 102, 0x41);
    write_bank(&f, 0, 0, 102, 0x51);
    CHECK(f.state.common.frame_seq == 1);
    CHECK(f.state.sweep.active && f.state.sweep.expected_y == 1);
    CHECK(f.state.sweep.data_bytes == 102);
    CHECK(!strcmp(f.trace.last_boundary, "origin_restart"));
    CHECK(!strcmp(f.trace.last_disposition, "committed"));
}

static void test_late_mismatch_is_fail_open(void) {
    fixture_t f;
    setup(&f);
    write_bank(&f, 0, 0, 102, 0x61);
    write_bank(&f, 1, 0, 101, 0x71);
    write_bank(&f, 3, 0, 1, 0x81);
    CHECK(f.state.common.frame_seq == 1 && f.trace.frames == 1);
    CHECK(f.trace.last_frame_bytes == 203);
    CHECK(f.state.sweep.active);
    CHECK(!strcmp(f.trace.last_disposition, "committed"));
    CHECK(!strcmp(f.trace.last_boundary, "fallback"));
    CHECK(f.state.presented_ddram[PCF8813_WIDTH] == 0x71);
}

static void test_seven_bank_c55_repaint(void) {
    fixture_t f;
    setup(&f);
    for (unsigned bank = 0; bank < 7; bank++)
        write_bank(&f, bank, 0, 102, (uint8_t)(bank + 1));
    CHECK(f.state.common.frame_seq == 0 && f.state.sweep.data_bytes == 714);

    f.state.x = 0;
    f.state.y = 0;
    set_select(&f, 1);
    cemu_pcf8813_write_byte(&f.periph, &f.soc, 1, 0x80);
    CHECK(f.state.common.frame_seq == 1 && f.trace.last_frame_bytes == 714);
    CHECK(f.state.presented_ddram[0] == 1);
    for (unsigned i = 1; i < 102; i++)
        cemu_pcf8813_write_byte(&f.periph, &f.soc, 1, (uint8_t)(0x80 + i));
    set_select(&f, 0);
    CHECK(f.state.common.frame_seq == 1);
    CHECK(f.state.sweep.active && f.state.sweep.data_bytes == 102);
    CHECK(!strcmp(f.trace.last_boundary, "origin_restart"));
    CHECK(!strcmp(f.trace.last_disposition, "committed"));
}

static void test_isolated_reset_and_run_end(void) {
    fixture_t f;
    setup(&f);
    write_bank(&f, 8, 4, 1, 0xFF);
    CHECK(f.state.common.frame_seq == 0 && f.state.sweep.active);
    CHECK(!strcmp(f.trace.last_disposition, "held"));

    setup(&f);
    write_bank(&f, 0, 0, 102, 0x81);
    cemu_pcf8813_reset(&f.periph, &f.soc);
    CHECK(f.state.common.frame_seq == 0 && !f.state.sweep.active);
    CHECK(f.state.sweep.data_bytes == 0);

    setup(&f);
    write_bank(&f, 0, 0, 102, 0x91);
    f.state.x = 0;
    f.state.y = 1;
    set_select(&f, 1);
    cemu_pcf8813_write_byte(&f.periph, &f.soc, 1, 0xA1);
    CHECK(cemu_pcf8813_flush_pending_frame(&f.periph, &f.soc) == 0);
    CHECK(f.state.common.frame_seq == 0 && f.trace.frames == 0);
    CHECK(f.state.common.transaction_data_bytes == 0);
    CHECK(!f.state.sweep.active && !f.state.sweep.transaction_started);
}

typedef struct { const char *name; void (*fn)(void); } entry_t;
static const entry_t TESTS[] = {
    {"full_sweep_and_render_hold", test_full_sweep_and_render_hold},
    {"early_coordinate_mode_and_restart",
     test_early_coordinate_mode_and_restart},
    {"late_mismatch_is_fail_open", test_late_mismatch_is_fail_open},
    {"seven_bank_c55_repaint", test_seven_bank_c55_repaint},
    {"isolated_reset_and_run_end", test_isolated_reset_and_run_end},
};

int main(void) {
    int n = (int)(sizeof TESTS / sizeof TESTS[0]);
    for (int i = 0; i < n; i++) {
        g_fail = 0;
        TESTS[i].fn();
        g_total_run++;
        g_total_fail += g_fail;
        if (g_fail) printf("[FAIL] %s (%d)\n", TESTS[i].name, g_fail);
    }
    printf("\n%d tests, %d failed\n", g_total_run, g_total_fail);
    return g_total_fail ? 1 : 0;
}
