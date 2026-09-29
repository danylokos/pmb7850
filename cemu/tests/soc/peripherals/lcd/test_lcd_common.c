/* Controller-neutral LCD wiring, transport, frame, and rendering tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lcd.h"
#include "lcd_common.h"
#include "soc.h"

static int g_fail, g_total_fail, g_total_run;
#define CHECK(cond) do { if (!(cond)) { g_fail++; \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define P3  0xFFC4u
#define P7  0xFFD0u
#define DP7 0xFFD2u

typedef struct {
    lcd_common_state_t common;
    lcd_sweep_tracker_t sweep;
    unsigned writes;
    unsigned selects;
    unsigned directions;
    unsigned falling_edges;
    unsigned consumes;
    unsigned closes;
    unsigned discards;
    unsigned presents;
    uint8_t last_byte;
    uint16_t last_tx;
    unsigned last_bits;
    int last_is_data;
    int last_msb_first;
} fake_lcd_t;

static void present(peripheral_t *p) {
    fake_lcd_t *st = p->state;
    st->presents++;
}

static void write_byte(peripheral_t *p, soc_t *s, int is_data, uint8_t value) {
    (void)s;
    fake_lcd_t *st = p->state;
    st->writes++;
    st->last_byte = value;
    st->last_is_data = is_data;
    cemu_lcd_common_note_data_byte(&st->common);
}

static void close_transaction(peripheral_t *p, soc_t *s) {
    (void)s;
    fake_lcd_t *st = p->state;
    st->closes++;
    st->common.transaction_data_bytes = 0;
}

static void discard_transaction(peripheral_t *p) {
    fake_lcd_t *st = p->state;
    st->discards++;
    st->common.transaction_data_bytes = 0;
}

static int ssc_supported(peripheral_t *p, unsigned frame_bits,
                         int msb_first, int is_data) {
    (void)p;
    (void)is_data;
    return frame_bits == 8 && msb_first;
}

static void ssc_consume(peripheral_t *p, soc_t *s, uint16_t tx,
                        unsigned frame_bits, int msb_first, int is_data) {
    (void)s;
    fake_lcd_t *st = p->state;
    st->consumes++;
    st->last_tx = tx;
    st->last_bits = frame_bits;
    st->last_msb_first = msb_first;
    st->last_is_data = is_data;
}

static void select_changed(peripheral_t *p, soc_t *s, int selected) {
    (void)s;
    fake_lcd_t *st = p->state;
    st->selects++;
    st->last_is_data = selected;
}

static void direction_changed(peripheral_t *p, soc_t *s, int output) {
    (void)s;
    fake_lcd_t *st = p->state;
    st->directions++;
    st->last_is_data = output;
}

static void input_falling(peripheral_t *p, soc_t *s) {
    (void)s;
    fake_lcd_t *st = p->state;
    st->falling_edges++;
}

static const lcd_controller_ops_t OPS = {
    .select_detail = "fake CS",
    .reset_detail = "fake reset",
    .frame_detail = "fake frame",
    .unsupported_detail = "fake unsupported",
    .data_active_high = 1,
    .track_reset = 1,
    .write_byte = write_byte,
    .ssc_supported = ssc_supported,
    .ssc_consume = ssc_consume,
    .select_changed = select_changed,
    .gpio_direction_changed = direction_changed,
    .gpio_input_falling_edge = input_falling,
};

static const lcd_controller_ops_t POLICY_OPS = {
    .select_detail = "fake CS",
    .reset_detail = "fake reset",
    .frame_detail = "fake frame",
    .unsupported_detail = "fake unsupported",
    .data_active_high = 1,
    .track_reset = 1,
    .write_byte = write_byte,
    .close_transaction = close_transaction,
    .discard_transaction = discard_transaction,
};

static const lcd_controller_ops_t SWEEP_OPS = {
    .frame_detail = "fake sweep",
    .present = present,
};

static device_lcd_config_t config(int bidirectional) {
    device_lcd_config_t cfg = {
        .model = "fake",
        .select_addr = P3,
        .select_bit = 11,
        .select_active_low = 1,
        .dc_addr = P7,
        .dc_bit = 6,
        .gpio_clock_addr = P7,
        .gpio_clock_bit = 5,
        .gpio_data_addr = P7,
        .gpio_data_direction_addr = bidirectional ? DP7 : 0,
        .gpio_data_bit = 7,
        .reset_capcom_channel = -1,
        .panel_width = 2,
        .panel_height = 1,
        .panel_origin_x = 1,
        .panel_origin_y = 1,
        .panel_mirror_x = 1,
    };
    return cfg;
}

static void set_pin(soc_t *s, uint32_t addr, int bit, int level) {
    uint16_t value = memory_controller_sfr_get(&s->memory, addr);
    if (level) value |= (uint16_t)(1u << bit);
    else value &= (uint16_t)~(1u << bit);
    cemu_memory_controller_sfr_put(&s->memory, addr, value);
}

static void init_fake(soc_t *s, peripheral_t *p, fake_lcd_t *st,
                      int bidirectional) {
    memset(s, 0, sizeof *s);
    memset(p, 0, sizeof *p);
    memset(st, 0, sizeof *st);
    device_lcd_config_t cfg = config(bidirectional);
    cemu_lcd_common_init(&st->common, &cfg, 4, 2);
    p->id = "lcd";
    p->model = "fake";
    p->state = st;
    set_pin(s, P3, 11, 1);
    set_pin(s, P7, 5, 0);
    if (bidirectional) set_pin(s, DP7, 7, 1);
    cemu_lcd_common_tick(p, s, 1, &st->common, &OPS);
}

static void clock_gpio_byte(soc_t *s, peripheral_t *p, fake_lcd_t *st,
                            uint8_t value) {
    for (int bit = 7; bit >= 0; bit--) {
        set_pin(s, P7, 7, (value >> bit) & 1u);
        set_pin(s, P7, 5, 1);
        cemu_lcd_common_tick(p, s, 1, &st->common, &OPS);
        set_pin(s, P7, 5, 0);
        cemu_lcd_common_tick(p, s, 1, &st->common, &OPS);
    }
}

static void test_gpio_selection_and_frame_closure(void) {
    soc_t s;
    peripheral_t p;
    fake_lcd_t st;
    init_fake(&s, &p, &st, 0);

    CHECK(st.common.panel_width == 2 && st.common.panel_height == 1);
    CHECK(st.common.panel_origin_x == 1 && st.common.panel_mirror_x);
    CHECK(!st.common.selected && st.common.select_known);
    CHECK(st.common.reset_known && !st.common.reset_asserted);

    set_pin(&s, P3, 11, 0);
    cemu_lcd_common_tick(&p, &s, 1, &st.common, &OPS);
    CHECK(st.common.selected && st.selects == 1);

    set_pin(&s, P7, 6, 1);
    clock_gpio_byte(&s, &p, &st, 0xA5);
    CHECK(st.writes == 1 && st.last_byte == 0xA5 && st.last_is_data);
    CHECK(st.common.transaction_data_bytes == 1);

    set_pin(&s, P3, 11, 1);
    cemu_lcd_common_tick(&p, &s, 1, &st.common, &OPS);
    CHECK(!st.common.selected && st.selects == 2);
    CHECK(st.common.frame_seq == 1 && st.common.transaction_data_bytes == 0);
}

static void test_direction_and_input_clock_callbacks(void) {
    soc_t s;
    peripheral_t p;
    fake_lcd_t st;
    init_fake(&s, &p, &st, 1);
    set_pin(&s, P3, 11, 0);
    cemu_lcd_common_tick(&p, &s, 1, &st.common, &OPS);
    CHECK(st.common.gpio_data_is_output && st.directions == 0);

    set_pin(&s, DP7, 7, 0);
    cemu_lcd_common_tick(&p, &s, 1, &st.common, &OPS);
    CHECK(!st.common.gpio_data_is_output && st.directions == 1);

    set_pin(&s, P7, 5, 1);
    cemu_lcd_common_tick(&p, &s, 1, &st.common, &OPS);
    set_pin(&s, P7, 5, 0);
    cemu_lcd_common_tick(&p, &s, 1, &st.common, &OPS);
    CHECK(st.falling_edges == 1);
}

static void test_ssc_lifecycle_and_rejection(void) {
    soc_t s;
    peripheral_t p;
    fake_lcd_t st;
    init_fake(&s, &p, &st, 0);
    set_pin(&s, P3, 11, 0);
    set_pin(&s, P7, 6, 1);

    cemu_lcd_common_ssc_start(&p, &s, 0x5A, 8, 1, &st.common, &OPS);
    CHECK(st.common.frame_pending && st.common.frame_selected);
    CHECK(cemu_lcd_common_ssc_complete(&p, &s, 0x5A, 8, 1,
                                  &st.common, &OPS) == 0xFF);
    CHECK(st.consumes == 1 && st.last_tx == 0x5A && st.last_bits == 8);
    CHECK(st.last_msb_first && st.last_is_data && !st.common.frame_pending);

    cemu_lcd_common_ssc_start(&p, &s, 0x7F, 7, 1, &st.common, &OPS);
    CHECK(st.common.unsupported_transfers == 1 && !st.common.frame_selected);
    cemu_lcd_common_ssc_abort(&st.common);
    CHECK(!st.common.frame_pending);
}

static void test_selectable_close_and_run_end_policy(void) {
    soc_t s;
    peripheral_t p;
    fake_lcd_t st;
    init_fake(&s, &p, &st, 0);
    set_pin(&s, P3, 11, 0);
    cemu_lcd_common_tick(&p, &s, 1, &st.common, &POLICY_OPS);
    st.common.transaction_data_bytes = 4;
    CHECK(cemu_lcd_common_flush_pending_frame(&p, &s, &st.common,
                                               &POLICY_OPS) == 0);
    CHECK(st.closes == 0 && st.discards == 1 &&
          st.common.transaction_data_bytes == 0);
    set_pin(&s, P3, 11, 1);
    cemu_lcd_common_tick(&p, &s, 1, &st.common, &POLICY_OPS);
    CHECK(st.closes == 0 && st.common.transaction_data_bytes == 0);
    CHECK(st.common.frame_seq == 0);

    st.common.transaction_data_bytes = 2;
    POLICY_OPS.discard_transaction(&p);
    CHECK(st.discards == 2 && st.common.transaction_data_bytes == 0);
}

static void test_lcd_state_append_and_legacy_migration(void) {
    lcd_state_storage_t storage;
    peripheral_t p;
    soc_t s;
    memset(&s, 0, sizeof s);

    device_lcd_config_t cfg = config(0);
    cfg.model = "hm17cm256";
    CHECK(cemu_lcd_periph_init(&p, &storage, &cfg));
    hm17cm256_state_t *hm = p.state;
    hm->gram[17] = 0xA5;
    hm->common.frame_seq = 9;
    uint8_t *legacy = malloc(HM17CM256_LEGACY_STATE_SIZE);
    CHECK(legacy != NULL);
    memcpy(legacy, hm, HM17CM256_LEGACY_STATE_SIZE);
    hm->presented_gram[17] = 0x5A;
    hm->sweep_active = 1;
    CHECK(cemu_lcd_restore_state(&p, &s, legacy,
                                 HM17CM256_LEGACY_STATE_SIZE, 1));
    CHECK(hm->gram[17] == 0xA5 && hm->presented_gram[17] == 0xA5);
    CHECK(hm->common.frame_seq == 9 && !hm->sweep_active);
    free(legacy);

    hm->presented_gram[17] = 0x33;
    hm->sweep_active = 1;
    hm->sweep_next_y = 12;
    size_t current_size = cemu_lcd_state_size(&p);
    uint8_t *current = malloc(current_size);
    CHECK(current != NULL);
    memcpy(current, hm, current_size);
    memset(hm, 0, current_size);
    CHECK(cemu_lcd_restore_state(&p, &s, current, current_size, 0));
    CHECK(hm->presented_gram[17] == 0x33 && hm->sweep_active);
    CHECK(hm->sweep_next_y == 12);
    free(current);

    hm->sweep_active = 1;
    hm->sweep_start_x = 13;
    hm->sweep_next_y = 7;
    hm->sweep_end_x = 44;
    hm->sweep_end_y = 81;
    hm->sweep_increment_x = 1;
    hm->sweep_data_bytes = 224;
    size_t hm37_size = cemu_lcd_snapshot_state_size(&p, 37);
    uint8_t *hm37 = malloc(hm37_size);
    CHECK(hm37 != NULL);
    memcpy(hm37, hm, hm37_size);
    memset(hm, 0, sizeof *hm);
    CHECK(cemu_lcd_restore_snapshot_state(&p, &s, hm37, hm37_size, 37));
    CHECK(hm->sweep.active && hm->sweep.expected_x == 13);
    CHECK(hm->sweep.expected_y == 7 && hm->sweep.data_bytes == 224);
    free(hm37);

    cfg.model = "pcf8813";
    CHECK(cemu_lcd_periph_init(&p, &storage, &cfg));
    pcf8813_state_t *pcf = p.state;
    pcf->ddram[29] = 0xC3;
    legacy = malloc(PCF8813_LEGACY_STATE_SIZE);
    CHECK(legacy != NULL);
    memcpy(legacy, pcf, PCF8813_LEGACY_STATE_SIZE);
    pcf->presented_ddram[29] = 0x3C;
    pcf->sweep_active = 1;
    CHECK(cemu_lcd_restore_state(&p, &s, legacy,
                                 PCF8813_LEGACY_STATE_SIZE, 1));
    CHECK(pcf->ddram[29] == 0xC3 && pcf->presented_ddram[29] == 0xC3);
    CHECK(!pcf->sweep_active);
    free(legacy);

    pcf->presented_ddram[29] = 0x69;
    pcf->sweep_active = 1;
    pcf->sweep_next_y = 5;
    current_size = cemu_lcd_state_size(&p);
    current = malloc(current_size);
    CHECK(current != NULL);
    memcpy(current, pcf, current_size);
    memset(pcf, 0, current_size);
    CHECK(cemu_lcd_restore_state(&p, &s, current, current_size, 0));
    CHECK(pcf->presented_ddram[29] == 0x69 && pcf->sweep_active);
    CHECK(pcf->sweep_next_y == 5);
    free(current);

    pcf->presented_ddram[29] = 0x42;
    pcf->sweep_active = 1;
    pcf->sweep_start_x = 0;
    pcf->sweep_next_y = 3;
    pcf->sweep_x_max = 101;
    pcf->sweep_y_max = 8;
    pcf->sweep_data_bytes = 306;
    size_t schema37_size = cemu_lcd_snapshot_state_size(&p, 37);
    uint8_t *schema37 = malloc(schema37_size);
    CHECK(schema37 != NULL);
    memcpy(schema37, pcf, schema37_size);
    memset(pcf, 0, sizeof *pcf);
    CHECK(cemu_lcd_restore_snapshot_state(&p, &s, schema37,
                                          schema37_size, 37));
    CHECK(pcf->presented_ddram[29] == 0x42 && pcf->sweep.active);
    CHECK(pcf->sweep.expected_y == 3 && pcf->sweep.data_bytes == 306);
    free(schema37);

    const char *immediate_models[] = {
        "hm17cm4096", "pcf8833-4wire", "s6b33bx"
    };
    for (unsigned i = 0; i < 3; i++) {
        cfg.model = immediate_models[i];
        CHECK(cemu_lcd_periph_init(&p, &storage, &cfg));
        size_t old_size = cemu_lcd_snapshot_state_size(&p, 37);
        uint8_t *old = malloc(old_size);
        CHECK(old != NULL);
        uint8_t *bytes = p.state;
        bytes[old_size - 1u] ^= 0x5A;
        memcpy(old, bytes, old_size);
        CHECK(cemu_lcd_restore_snapshot_state(&p, &s, old, old_size, 37));
        if (!strcmp(p.model, "hm17cm4096")) {
            hm17cm4096_state_t *st = p.state;
            CHECK(!memcmp(st->gram, st->presented_gram, sizeof st->gram));
            CHECK(!st->sweep.active);
        } else if (!strcmp(p.model, "pcf8833-4wire")) {
            pcf8833_4wire_state_t *st = p.state;
            CHECK(!memcmp(st->gram, st->presented_gram, sizeof st->gram));
            CHECK(!st->sweep.active);
        } else {
            s6b33bx_state_t *st = p.state;
            CHECK(!memcmp(st->gram, st->presented_gram, sizeof st->gram));
            CHECK(!st->sweep.active);
        }
        free(old);
    }
}

static void pixel(const void *state, unsigned x, unsigned y,
                  int raw, uint8_t out[3]) {
    (void)state;
    out[0] = (uint8_t)x;
    out[1] = (uint8_t)y;
    out[2] = (uint8_t)raw;
}

static void test_render_geometry_and_scaling(void) {
    lcd_common_state_t st;
    device_lcd_config_t cfg = config(0);
    cemu_lcd_common_init(&st, &cfg, 4, 2);
    uint8_t rgb[4u * 2u * 2u * 2u * 3u];
    memset(rgb, 0xFF, sizeof rgb);
    CHECK(cemu_lcd_common_render_rgb(&st, &st, 0, 4, 2, 2,
                                rgb, sizeof rgb, pixel) == 0);
    CHECK(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0);
    CHECK(rgb[6] == 1 && rgb[7] == 0 && rgb[8] == 0);
    CHECK(cemu_lcd_common_render_rgb(&st, &st, 1, 4, 2, 1,
                                rgb, sizeof rgb, pixel) == 0);
    CHECK(rgb[2] == 1 && rgb[3] == 1 && rgb[4] == 0 && rgb[5] == 1);
}

static void sweep_write(peripheral_t *p, soc_t *s, fake_lcd_t *st,
                        unsigned x, unsigned next_x, unsigned phase_before,
                        unsigned phase_after, uint32_t mode, int terminal) {
    lcd_sweep_write_t write = {
        .x = x, .y = 0, .next_x = next_x, .next_y = 0,
        .start_x = 0, .start_y = 0, .end_x = 1, .end_y = 0,
        .mode = mode, .pack_phase = phase_before,
        .storage_writes = phase_after == 0, .data_bytes = 1,
        .windowed = 1, .terminal = terminal,
    };
    cemu_lcd_sweep_prepare_write(p, s, &st->common, &st->sweep, &write,
                                 &SWEEP_OPS);
    write.pack_phase = phase_after;
    cemu_lcd_sweep_finish_write(p, s, &st->common, &st->sweep, &write,
                                &SWEEP_OPS);
}

static void test_sweep_lifecycle(void) {
    soc_t s;
    peripheral_t p;
    fake_lcd_t st;
    memset(&s, 0, sizeof s);
    memset(&p, 0, sizeof p);
    memset(&st, 0, sizeof st);
    p.state = &st;

    sweep_write(&p, &s, &st, 0, 1, 0, 0, 7, 0);
    cemu_lcd_sweep_close_transaction(&p, &s, &st.common, &st.sweep,
                                     &SWEEP_OPS, "fake");
    CHECK(st.sweep.active && st.common.frame_seq == 0 && st.presents == 0);
    sweep_write(&p, &s, &st, 1, 0, 0, 0, 7, 1);
    CHECK(!st.sweep.active && st.common.frame_seq == 1 && st.presents == 1);
    CHECK(st.sweep.transaction_sequence == 1);

    cemu_lcd_sweep_close_transaction(&p, &s, &st.common, &st.sweep,
                                     &SWEEP_OPS, "fake");
    sweep_write(&p, &s, &st, 0, 1, 0, 0, 7, 0);
    cemu_lcd_sweep_close_transaction(&p, &s, &st.common, &st.sweep,
                                     &SWEEP_OPS, "fake");
    sweep_write(&p, &s, &st, 0, 1, 0, 0, 7, 0);
    CHECK(st.common.frame_seq == 2 && st.presents == 2);

    cemu_lcd_sweep_close_transaction(&p, &s, &st.common, &st.sweep,
                                     &SWEEP_OPS, "fake");
    sweep_write(&p, &s, &st, 1, 0, 0, 0, 8, 0);
    CHECK(st.common.frame_seq == 3 && st.sweep.active);

    cemu_lcd_sweep_discard_transaction(&st.common, &st.sweep);
    CHECK(!st.sweep.active && st.common.frame_seq == 3);

    sweep_write(&p, &s, &st, 0, 1, 0, 1, 9, 0);
    CHECK(st.sweep.active && st.sweep.pack_phase == 1 &&
          st.sweep.storage_writes == 0);
    sweep_write(&p, &s, &st, 1, 0, 1, 0, 9, 1);
    CHECK(!st.sweep.active && st.common.frame_seq == 4);

    lcd_sweep_tracker_t checkpoint = st.sweep;
    checkpoint.active = 1;
    checkpoint.data_bytes = 4;
    st.sweep = checkpoint;
    CHECK(!memcmp(&st.sweep, &checkpoint, sizeof checkpoint));
}

typedef struct { const char *name; void (*fn)(void); } entry_t;
static const entry_t TESTS[] = {
    {"gpio_selection_and_frame_closure", test_gpio_selection_and_frame_closure},
    {"direction_and_input_clock_callbacks", test_direction_and_input_clock_callbacks},
    {"ssc_lifecycle_and_rejection", test_ssc_lifecycle_and_rejection},
    {"selectable_close_and_run_end_policy",
     test_selectable_close_and_run_end_policy},
    {"lcd_state_append_and_legacy_migration",
     test_lcd_state_append_and_legacy_migration},
    {"render_geometry_and_scaling", test_render_geometry_and_scaling},
    {"sweep_lifecycle", test_sweep_lifecycle},
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
