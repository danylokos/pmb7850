/* HM17CM256 atomic repaint publication tests. */
#include <stdio.h>
#include <string.h>

#include "hm17cm256.h"
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
    hm17cm256_state_t state;
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
        .model = "hm17cm256",
        .select_addr = P3,
        .select_bit = 11,
        .select_active_low = 1,
        .dc_addr = 0xFFD0,
        .dc_bit = 6,
        .reset_capcom_channel = -1,
        .panel_width = 128,
        .panel_height = 80,
    };
    cemu_event_hub_init(&f->soc.instrumentation);
    cemu_event_subscribe_filtered(&f->soc.instrumentation,
                                  CEMU_EVENT_PERIPHERAL,
                                  trace_consumer, trace_filter, &f->trace);
    cemu_hm17cm256_periph_init(&f->periph, &f->state, &cfg);
    set_select(f, 0);
    f->state.hm17.increment_x = 1;
    f->state.hm17.increment_y = 0;
    f->state.hm17.fixed_pwm = 1;
    f->state.hm17.display_on = 1;
}

static void write_row(fixture_t *f, unsigned row, unsigned x,
                      unsigned bytes, uint8_t value) {
    f->state.hm17.x = (uint8_t)x;
    f->state.hm17.y = (uint8_t)row;
    set_select(f, 1);
    for (unsigned i = 0; i < bytes; i++)
        cemu_hm17cm256_write_byte(&f->periph, &f->soc, 1,
                                  (uint8_t)(value + i));
    set_select(f, 0);
}

static void test_full_101_byte_sweep_and_render_hold(void) {
    fixture_t f;
    setup(&f);
    write_row(&f, 0, 0, 101, 0xE1);
    CHECK(f.state.gram[0] == 0xE1);
    CHECK(f.state.presented_gram[0] == 0);
    CHECK(f.state.common.data_seq == 101);
    CHECK(f.state.common.frame_seq == 0 && f.trace.frames == 0);
    CHECK(f.state.sweep.active && f.state.sweep.expected_y == 1);
    CHECK(!strcmp(f.trace.last_disposition, "held"));

    uint8_t visible[128u * 80u * 3u];
    uint8_t raw[HM17CM256_GRAM_PIXELS * 3u];
    CHECK(cemu_hm17cm256_render_rgb(&f.state, 0, 1, visible,
                                    sizeof visible) == 0);
    CHECK(cemu_hm17cm256_render_rgb(&f.state, 1, 1, raw,
                                    sizeof raw) == 0);
    CHECK(visible[0] == 0 && visible[1] == 0 && visible[2] == 0);
    CHECK(raw[0] != 0 || raw[1] != 0 || raw[2] != 0);

    for (unsigned row = 1; row < 80; row++)
        write_row(&f, row, 0, 101, (uint8_t)(row + 1));
    CHECK(f.state.common.frame_seq == 1 && f.trace.frames == 1);
    CHECK(f.trace.last_frame_bytes == 8080);
    CHECK(f.state.transaction_count == 80 && f.trace.transactions == 80);
    CHECK(f.trace.last_sequence == 80 && f.trace.last_transaction_bytes == 101);
    CHECK(f.trace.last_x == 0 && f.trace.last_y == 79);
    CHECK(!strcmp(f.trace.last_disposition, "committed"));
    CHECK(!strcmp(f.trace.last_boundary, "viewport_complete"));
    CHECK(!f.state.sweep.active && f.state.sweep.data_bytes == 0);
    CHECK(!memcmp(f.state.gram, f.state.presented_gram,
                  sizeof f.state.gram));
}

static void test_32_byte_sweep(void) {
    fixture_t f;
    setup(&f);
    for (unsigned row = 0; row < 80; row++)
        write_row(&f, row, 13, 32, (uint8_t)(row + 3));
    CHECK(f.state.common.frame_seq == 1);
    CHECK(f.trace.last_frame_bytes == 2560);
    CHECK(f.state.common.data_seq == 2560);
    CHECK(!strcmp(f.trace.last_disposition, "committed"));
}

static void test_early_interruption_and_restart(void) {
    fixture_t f;
    setup(&f);
    write_row(&f, 0, 0, 101, 0x11);
    f.state.hm17.x = 0;
    f.state.hm17.y = 2;
    set_select(&f, 1);
    cemu_hm17cm256_write_byte(&f.periph, &f.soc, 1, 0x77);
    CHECK(f.state.common.frame_seq == 1 && f.trace.last_frame_bytes == 101);
    CHECK(f.state.presented_gram[0] == 0x11);
    CHECK(f.state.presented_gram[2u * HM17CM256_WIDTH] == 0);
    set_select(&f, 0);
    CHECK(f.state.common.frame_seq == 1);
    CHECK(!strcmp(f.trace.last_disposition, "committed"));

    setup(&f);
    write_row(&f, 0, 0, 101, 0x18);
    f.state.hm17.x = 0;
    f.state.hm17.y = 1;
    f.state.hm17.increment_x = 0;
    f.state.hm17.increment_y = 1;
    set_select(&f, 1);
    cemu_hm17cm256_write_byte(&f.periph, &f.soc, 1, 0x28);
    CHECK(f.state.common.frame_seq == 1 && f.trace.last_frame_bytes == 101);
    CHECK(f.state.sweep.active);
    set_select(&f, 0);

    setup(&f);
    write_row(&f, 0, 0, 101, 0x21);
    write_row(&f, 0, 0, 101, 0x31);
    CHECK(f.state.common.frame_seq == 1);
    CHECK(f.state.sweep.active && f.state.sweep.expected_y == 1);
    CHECK(f.state.sweep.data_bytes == 101);
    CHECK(!strcmp(f.trace.last_boundary, "origin_restart"));
    CHECK(!strcmp(f.trace.last_disposition, "committed"));
}

static void test_late_mismatch_is_fail_open(void) {
    fixture_t f;
    setup(&f);
    write_row(&f, 0, 0, 101, 0x41);
    write_row(&f, 1, 0, 100, 0x51);
    CHECK(f.state.common.frame_seq == 1 && f.trace.frames == 1);
    CHECK(f.trace.last_frame_bytes == 201);
    CHECK(!f.state.sweep.active);
    CHECK(!strcmp(f.trace.last_disposition, "committed"));
    CHECK(!strcmp(f.trace.last_boundary, "fallback"));
    CHECK(f.state.presented_gram[HM17CM256_WIDTH] == 0x51);
}

static void test_isolated_reset_and_run_end(void) {
    fixture_t f;
    setup(&f);
    write_row(&f, 10, 4, 1, 0xE0);
    CHECK(f.state.common.frame_seq == 0 && f.state.sweep.active);
    CHECK(!strcmp(f.trace.last_disposition, "held"));

    setup(&f);
    write_row(&f, 0, 0, 101, 0x61);
    cemu_hm17cm256_reset(&f.periph, &f.soc);
    CHECK(f.state.common.frame_seq == 0 && !f.state.sweep.active);
    CHECK(f.state.sweep.data_bytes == 0);

    setup(&f);
    write_row(&f, 0, 0, 101, 0x71);
    f.state.hm17.x = 0;
    f.state.hm17.y = 1;
    set_select(&f, 1);
    cemu_hm17cm256_write_byte(&f.periph, &f.soc, 1, 0x81);
    CHECK(cemu_hm17cm256_flush_pending_frame(&f.periph, &f.soc) == 0);
    CHECK(f.state.common.frame_seq == 0 && f.trace.frames == 0);
    CHECK(f.state.common.transaction_data_bytes == 0);
    CHECK(!f.state.sweep.active && !f.state.sweep.transaction_started);
}

typedef struct { const char *name; void (*fn)(void); } entry_t;
static const entry_t TESTS[] = {
    {"full_101_byte_sweep_and_render_hold",
     test_full_101_byte_sweep_and_render_hold},
    {"32_byte_sweep", test_32_byte_sweep},
    {"early_interruption_and_restart", test_early_interruption_and_restart},
    {"late_mismatch_is_fail_open", test_late_mismatch_is_fail_open},
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
