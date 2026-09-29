/* S6B33Bx-compatible LCD transport and framebuffer. */
#include <string.h>

#include "s6b33bx.h"
#include "soc.h"

static const lcd_controller_ops_t S6B33BX_OPS;

/*
 * Only 0x42/0x43 parameters have semantic state below. Other bytes are
 * whitelisted from the calibrated command stream so tracing can distinguish
 * known traffic from new traffic without pretending to model each family
 * register. In particular, the calibrated stream contains 0x10 0x35; the B2
 * comparison manual decodes that parameter as 1/96 duty with a reserved bit
 * set, which does not explain the 130-row panel.
 */
static int calibrated_control_byte(uint8_t value) {
    switch (value) {
    case 0x00: case 0x01: case 0x02: case 0x05:
    case 0x0F: case 0x10: case 0x20: case 0x22:
    case 0x24: case 0x26: case 0x28: case 0x2A:
    case 0x2C: case 0x30: case 0x32: case 0x34:
    case 0x35: case 0x40: case 0x42: case 0x43:
    case 0x51: case 0x80: case 0x81: case 0x82:
    case 0xAF:
        return 1;
    default:
        return 0;
    }
}

static void trace_command_extra(cemu_native_trace_event_t *ev, const void *state) {
    const s6b33bx_state_t *st = state;
    cemu_event_field_i64(&ev->info, "count", st->command_count);
    cemu_event_field_i64(&ev->info, "window_start_x", st->window_start_x);
    cemu_event_field_i64(&ev->info, "window_end_x", st->window_end_x);
    cemu_event_field_i64(&ev->info, "window_start_y", st->window_start_y);
    cemu_event_field_i64(&ev->info, "window_end_y", st->window_end_y);
    cemu_event_field_i64(&ev->info, "write_index", st->write_index);
    cemu_event_field_i64(&ev->info, "pending_window_command",
                   st->pending_window_command);
    cemu_event_field_i64(&ev->info, "pending_window_index",
                   st->pending_window_index);
}

static uint8_t bounded_coordinate(uint8_t value) {
    return value < S6B33BX_WIDTH ? value : S6B33BX_WIDTH - 1u;
}

static void write_window_parameter(peripheral_t *p, soc_t *s,
                                   s6b33bx_state_t *st, uint8_t value) {
    uint8_t coordinate = bounded_coordinate(value);
    /*
     * The B2 reference maps controller X (0x42, 162 commons) vertically and
     * controller Y (0x43, 132 RGB segments) horizontally. Non-square update
     * traces prove the same mapping, with visible X advancing first.
     */
    uint8_t *start = st->pending_window_command == 0x43
                   ? &st->window_start_x : &st->window_start_y;
    uint8_t *end = st->pending_window_command == 0x43
                 ? &st->window_end_x : &st->window_end_y;
    if (st->pending_window_index++ == 0) {
        st->pending_window_start = coordinate;
    } else {
        uint8_t new_end = coordinate < st->pending_window_start
                        ? st->pending_window_start : coordinate;
        if ((*start != st->pending_window_start || *end != new_end) &&
            st->sweep.active)
            cemu_lcd_sweep_fail_open(p, s, &st->common, &st->sweep,
                                     &S6B33BX_OPS);
        *start = st->pending_window_start;
        *end = new_end;
        st->pending_window_command = 0;
        st->pending_window_index = 0;
        st->pending_window_start = 0;
    }
    st->write_index = 0;
}

void cemu_s6b33bx_write_command(peripheral_t *p, soc_t *s, uint8_t value) {
    s6b33bx_state_t *st = p->state;
    int parameter = st->pending_window_command != 0;
    int known = parameter || calibrated_control_byte(value);
    st->last_command = value;
    st->command_count++;
    if (!known) st->unknown_commands++;
    if (parameter)
        write_window_parameter(p, s, st, value);
    else if (value == 0x42 || value == 0x43) {
        st->pending_window_command = value;
        st->pending_window_index = 0;
    }
    cemu_lcd_common_emit_command(&st->common, s, st, value, known,
                            parameter ? "S6B33Bx window parameter"
                            : known ? "calibrated S6B33Bx control"
                                  : "unknown S6B33Bx control",
                            trace_command_extra);
}

void cemu_s6b33bx_write_pixel(peripheral_t *p, soc_t *s, uint16_t value) {
    s6b33bx_state_t *st = p->state;
    unsigned width = st->window_end_x - st->window_start_x + 1u;
    unsigned height = st->window_end_y - st->window_start_y + 1u;
    unsigned window_pixels = width * height;
    unsigned offset = st->write_index < window_pixels ? st->write_index : 0u;
    unsigned x = st->window_start_x + offset % width;
    unsigned y = st->window_start_y + offset / width;
    unsigned next_offset = offset + 1u >= window_pixels ? 0u : offset + 1u;
    lcd_sweep_write_t write = {
        .x = x, .y = y,
        .next_x = st->window_start_x + next_offset % width,
        .next_y = st->window_start_y + next_offset / width,
        .start_x = st->window_start_x, .start_y = st->window_start_y,
        .end_x = st->window_end_x, .end_y = st->window_end_y,
        .storage_writes = 1, .data_bytes = 2, .windowed = 1,
        .terminal = next_offset == 0,
    };
    cemu_lcd_sweep_prepare_write(p, s, &st->common, &st->sweep, &write,
                                 &S6B33BX_OPS);
    if (x < S6B33BX_WIDTH && y < S6B33BX_HEIGHT) {
        st->gram[y * S6B33BX_WIDTH + x] = value;
        cemu_lcd_common_note_pixel(&st->common, s);
        cemu_lcd_common_emit_data(&st->common, s, st, value, 2, x, y,
                             "S6B33Bx 16-bit GRAM",
                             NULL);
    }
    st->write_index = next_offset;
    cemu_lcd_sweep_finish_write(p, s, &st->common, &st->sweep, &write,
                                &S6B33BX_OPS);
}

static void write_byte(peripheral_t *p, soc_t *s,
                       int is_data, uint8_t value) {
    s6b33bx_state_t *st = p->state;
    if (is_data) {
        cemu_lcd_common_emit_unsupported(&st->common, s, 8, 1,
                                    &S6B33BX_OPS);
        return;
    }
    cemu_s6b33bx_write_command(p, s, value);
}

static void present(peripheral_t *p) {
    s6b33bx_state_t *st = p->state;
    memcpy(st->presented_gram, st->gram, sizeof st->presented_gram);
}

static void close_transaction(peripheral_t *p, soc_t *s) {
    s6b33bx_state_t *st = p->state;
    cemu_lcd_sweep_close_transaction(p, s, &st->common, &st->sweep,
                                     &S6B33BX_OPS, "s6b33bx");
}

static void discard_transaction(peripheral_t *p) {
    s6b33bx_state_t *st = p->state;
    cemu_lcd_sweep_discard_transaction(&st->common, &st->sweep);
}

static void clear_partial(peripheral_t *p) {
    s6b33bx_state_t *st = p->state;
    st->pending_window_command = 0;
    st->pending_window_index = 0;
    st->pending_window_start = 0;
}

static int ssc_supported(peripheral_t *p, unsigned frame_bits,
                         int msb_first, int is_data) {
    (void)p;
    /*
     * The B2 reference serial input is byte-oriented. The calibrated transport
     * groups each big-endian RGB565 byte pair into one 16-bit transfer, so this
     * endpoint intentionally accepts only that measured data form.
     */
    return msb_first &&
           ((!is_data && frame_bits == 8) ||
            (is_data && frame_bits == 16));
}

static void ssc_consume(peripheral_t *p, soc_t *s, uint16_t tx,
                        unsigned frame_bits, int msb_first, int is_data) {
    (void)frame_bits;
    (void)msb_first;
    if (is_data)
        cemu_s6b33bx_write_pixel(p, s, tx);
    else
        cemu_s6b33bx_write_command(p, s, (uint8_t)tx);
}

static void tick(peripheral_t *p, soc_t *s, int n) {
    s6b33bx_state_t *st = p->state;
    cemu_lcd_common_tick(p, s, n, &st->common, &S6B33BX_OPS);
}

static uint64_t next_event(peripheral_t *p, soc_t *s) {
    s6b33bx_state_t *st = p->state;
    return cemu_lcd_common_next_event(p, s, &st->common, &S6B33BX_OPS);
}

void cemu_s6b33bx_ssc_start(void *ctx, soc_t *s, uint16_t tx,
                       unsigned frame_bits, int msb_first) {
    peripheral_t *p = ctx;
    s6b33bx_state_t *st = p->state;
    cemu_lcd_common_ssc_start(p, s, tx, frame_bits, msb_first,
                         &st->common, &S6B33BX_OPS);
}

uint16_t cemu_s6b33bx_ssc_complete(void *ctx, soc_t *s, uint16_t tx,
                              unsigned frame_bits, int msb_first) {
    peripheral_t *p = ctx;
    s6b33bx_state_t *st = p->state;
    return cemu_lcd_common_ssc_complete(p, s, tx, frame_bits, msb_first,
                                   &st->common, &S6B33BX_OPS);
}

void cemu_s6b33bx_ssc_abort(void *ctx, soc_t *s) {
    (void)s;
    peripheral_t *p = ctx;
    s6b33bx_state_t *st = p->state;
    cemu_lcd_common_ssc_abort(&st->common);
}

int cemu_s6b33bx_flush_pending_frame(peripheral_t *p, soc_t *s) {
    s6b33bx_state_t *st = p->state;
    return cemu_lcd_common_flush_pending_frame(p, s, &st->common,
                                          &S6B33BX_OPS);
}

static uint8_t expand5(unsigned value) {
    return (uint8_t)((value << 3) | (value >> 2));
}

static uint8_t expand6(unsigned value) {
    return (uint8_t)((value << 2) | (value >> 4));
}

static void pixel_rgb(const void *state, unsigned x, unsigned y,
                      int raw, uint8_t out[3]) {
    const s6b33bx_state_t *st = state;
    const uint16_t *memory = raw ? st->gram : st->presented_gram;
    uint16_t pixel = memory[y * S6B33BX_WIDTH + x];
    out[0] = expand5((pixel >> 11) & 0x1Fu);
    out[1] = expand6((pixel >> 5) & 0x3Fu);
    out[2] = expand5(pixel & 0x1Fu);
}

int cemu_s6b33bx_render_rgb(const s6b33bx_state_t *st, int raw,
                       unsigned scale, uint8_t *rgb, size_t rgb_len) {
    return st ? cemu_lcd_common_render_rgb(&st->common, st, raw,
                                       S6B33BX_WIDTH, S6B33BX_HEIGHT,
                                       scale, rgb, rgb_len, pixel_rgb) : -1;
}

void cemu_s6b33bx_periph_init(peripheral_t *p, s6b33bx_state_t *st,
                         const device_lcd_config_t *cfg) {
    memset(p, 0, sizeof *p);
    memset(st, 0, sizeof *st);
    cemu_lcd_common_init(&st->common, cfg, S6B33BX_WIDTH, S6B33BX_HEIGHT);
    st->window_end_x = S6B33BX_WIDTH - 1u;
    st->window_end_y = S6B33BX_HEIGHT - 1u;

    p->id = "lcd";
    p->model = "s6b33bx";
    p->state = st;
    p->tick = tick;
    p->next_event_ticks = next_event;
    p->advance_quiet = cemu_lcd_common_advance_quiet;
}

static const lcd_controller_ops_t S6B33BX_OPS = {
    .select_detail = "S6B33Bx chip select",
    .reset_detail = "S6B33Bx reset",
    .frame_detail = "S6B33Bx-compatible 16-bit frame",
    .unsupported_detail = "unsupported S6B33Bx transfer",
    .data_active_high = 1,
    .clear_partial = clear_partial,
    .write_byte = write_byte,
    .ssc_supported = ssc_supported,
    .ssc_consume = ssc_consume,
    .close_transaction = close_transaction,
    .discard_transaction = discard_transaction,
    .present = present,
};
