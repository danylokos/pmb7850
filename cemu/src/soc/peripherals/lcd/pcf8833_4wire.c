/*
 * PCF8833-compatible command and GRAM behavior on SL55's measured four-wire
 * attachment. The exact fitted die remains open: unlike the PCF8833's
 * documented 9-clock serial protocol, this board sends 8 clocks plus D/C.
 */
#include <string.h>

#include "pcf8833_4wire.h"
#include "soc.h"

static const lcd_controller_ops_t PCF8833_4WIRE_OPS;

enum {
    CMD_SWRESET = 0x01,
    CMD_SLPIN = 0x10,
    CMD_SLPOUT = 0x11,
    CMD_PTLON = 0x12,
    CMD_NORON = 0x13,
    CMD_INVOFF = 0x20,
    CMD_INVON = 0x21,
    CMD_DALO = 0x22,
    CMD_DAL = 0x23,
    CMD_SETCON = 0x25,
    CMD_DISPOFF = 0x28,
    CMD_DISPON = 0x29,
    CMD_CASET = 0x2A,
    CMD_PASET = 0x2B,
    CMD_RAMWR = 0x2C,
    CMD_RGBSET = 0x2D,
    CMD_PTLAR = 0x30,
    CMD_VSCRDEF = 0x33,
    CMD_TEOFF = 0x34,
    CMD_TEON = 0x35,
    CMD_MADCTL = 0x36,
    CMD_SEP = 0x37,
    CMD_IDMOFF = 0x38,
    CMD_IDMON = 0x39,
    CMD_COLMOD = 0x3A,
};

enum {
    MADCTL_MY = 0x80,
    MADCTL_MX = 0x40,
    MADCTL_VERTICAL = 0x20,
    MADCTL_BGR = 0x08,
};

static void reset_registers(pcf8833_4wire_state_t *st) {
    st->x = st->window_start_x = 2;
    st->y = st->window_start_y = 2;
    st->end_x = 129;
    st->end_y = 129;
    st->display_on = 1;
    st->reverse = 0;
    cemu_lcd_common_reset_serial(&st->common);
    cemu_lcd_sweep_reset(&st->common, &st->sweep);

    st->current_command = 0;
    st->parameter_index = 0;
    st->colmod = 3;
    st->madctl = 0;
    st->sleep_out = 0;
    st->partial_mode = 0;
    st->normal_mode = 1;
    st->idle_mode = 0;
    st->tearing_on = 0;
    st->booster_on = 1;
    st->ram_write = 0;
    st->all_pixels = 0;
    st->contrast = 0;
    st->partial_start = 0;
    st->partial_end = 31;
    st->scroll_definition[0] = 0;
    st->scroll_definition[1] = 130;
    st->scroll_definition[2] = 0;
    st->scroll_entry = 0;
    static const uint8_t DEFAULT_LUT[20] = {
        0, 2, 4, 6, 9, 11, 13, 15,
        0, 2, 4, 6, 9, 11, 13, 15,
        0, 4, 11, 15,
    };
    memcpy(st->rgb_lut, DEFAULT_LUT, sizeof DEFAULT_LUT);
    st->rgb_lut_count = 0;
}

void cemu_pcf8833_4wire_reset(peripheral_t *p, soc_t *s) {
    pcf8833_4wire_state_t *st = p->state;
    reset_registers(st);
    cemu_lcd_common_emit_reset(&st->common, s, 1, &PCF8833_4WIRE_OPS);
}

static uint8_t bounded_address(uint8_t value) {
    return value < PCF8833_4WIRE_WIDTH
         ? value : (uint8_t)(PCF8833_4WIRE_WIDTH - 1u);
}

static const char *begin_command(peripheral_t *p, uint8_t value, int *known) {
    pcf8833_4wire_state_t *st = p->state;
    *known = 1;
    st->current_command = value;
    st->parameter_index = 0;
    st->ram_write = 0;

    switch (value) {
    case 0x00: return "nop";
    case CMD_SWRESET:
        reset_registers(st);
        st->current_command = CMD_SWRESET;
        return "software_reset";
    case 0x02: st->booster_on = 0; return "booster_off";
    case 0x03: st->booster_on = 1; return "booster_on";
    case CMD_SLPIN: st->sleep_out = 0; return "sleep_in";
    case CMD_SLPOUT: st->sleep_out = 1; return "sleep_out";
    case CMD_PTLON:
        st->partial_mode = 1; st->normal_mode = 0; return "partial_mode_on";
    case CMD_NORON:
        st->partial_mode = 0; st->normal_mode = 1; st->all_pixels = 0;
        return "normal_mode_on";
    case CMD_INVOFF: st->reverse = 0; return "inversion_off";
    case CMD_INVON: st->reverse = 1; return "inversion_on";
    case CMD_DALO: st->all_pixels = 2; return "all_pixels_off";
    case CMD_DAL: st->all_pixels = 1; return "all_pixels_on";
    case CMD_SETCON: return "set_contrast";
    case CMD_DISPOFF: st->display_on = 0; return "display_off";
    case CMD_DISPON: st->display_on = 1; return "display_on";
    case CMD_CASET: return "column_address_set";
    case CMD_PASET: return "page_address_set";
    case CMD_RAMWR:
        st->x = st->window_start_x;
        st->y = st->window_start_y;
        st->ram_write = 1;
        return "memory_write";
    case CMD_RGBSET: st->rgb_lut_count = 0; return "colour_set";
    case CMD_PTLAR: return "partial_area";
    case CMD_VSCRDEF: return "vertical_scroll_definition";
    case CMD_TEOFF: st->tearing_on = 0; return "tearing_line_off";
    case CMD_TEON: st->tearing_on = 1; return "tearing_line_on";
    case CMD_MADCTL: return "memory_data_access_control";
    case CMD_SEP: return "scroll_entry_point";
    case CMD_IDMOFF: st->idle_mode = 0; return "idle_mode_off";
    case CMD_IDMON: st->idle_mode = 1; return "idle_mode_on";
    case CMD_COLMOD: return "interface_pixel_format";
    default:
        *known = 0;
        st->unknown_commands++;
        return "unknown_vendor_command";
    }
}

static void apply_parameter(peripheral_t *p, soc_t *s, uint8_t value) {
    pcf8833_4wire_state_t *st = p->state;
    unsigned index = st->parameter_index;
    int conflict = 0;
    if (st->current_command == CMD_CASET && index < 2)
        conflict = bounded_address(value) !=
            (index ? st->end_x : st->window_start_x);
    else if (st->current_command == CMD_PASET && index < 2)
        conflict = bounded_address(value) !=
            (index ? st->end_y : st->window_start_y);
    else if (st->current_command == CMD_MADCTL && index == 0)
        conflict = value != st->madctl;
    else if (st->current_command == CMD_COLMOD && index == 0 &&
             (value & 7u))
        conflict = (value & 7u) != st->colmod;
    if (conflict)
        cemu_lcd_sweep_fail_open(p, s, &st->common, &st->sweep,
                                 &PCF8833_4WIRE_OPS);
    switch (st->current_command) {
    case CMD_SETCON:
        if (index == 0) st->contrast = value & 0x7Fu;
        break;
    case CMD_CASET:
        if (index == 0) st->window_start_x = bounded_address(value);
        else if (index == 1) st->end_x = bounded_address(value);
        break;
    case CMD_PASET:
        if (index == 0) st->window_start_y = bounded_address(value);
        else if (index == 1) st->end_y = bounded_address(value);
        break;
    case CMD_RGBSET:
        if (index < sizeof st->rgb_lut) {
            st->rgb_lut[index] = value & 0x0Fu;
            st->rgb_lut_count = (uint8_t)(index + 1u);
        }
        break;
    case CMD_PTLAR:
        if (index == 0) st->partial_start = bounded_address(value);
        else if (index == 1) st->partial_end = bounded_address(value);
        break;
    case CMD_VSCRDEF:
        if (index < 3) st->scroll_definition[index] = value;
        break;
    case CMD_MADCTL:
        if (index == 0) {
            st->madctl = value;
        }
        break;
    case CMD_SEP:
        if (index == 0) st->scroll_entry = value;
        break;
    case CMD_COLMOD:
        if (index == 0 && (value & 7u)) st->colmod = value & 7u;
        break;
    default:
        break;
    }
    st->parameter_index++;
}

static void trace_command_extra(cemu_native_trace_event_t *ev, const void *state) {
    const pcf8833_4wire_state_t *st = state;
    cemu_event_field_i64(&ev->info, "x", st->x);
    cemu_event_field_i64(&ev->info, "y", st->y);
    cemu_event_field_bool(&ev->info, "reset", st->common.reset_asserted);
}

static void trace_parameter_extra(cemu_native_trace_event_t *ev, const void *state) {
    const pcf8833_4wire_state_t *st = state;
    cemu_event_field_i64(&ev->info, "command", st->current_command);
    cemu_event_field_i64(&ev->info, "parameter_index",
                   st->parameter_index ? st->parameter_index - 1u : 0u);
}

static void trace_pixel_extra(cemu_native_trace_event_t *ev, const void *state) {
    const pcf8833_4wire_state_t *st = state;
    cemu_event_field_i64(&ev->info, "colmod", st->colmod);
    cemu_event_field_i64(&ev->info, "madctl", st->madctl);
    cemu_event_field_bool(&ev->info, "bgr_recorded_not_applied",
                    !!(st->madctl & MADCTL_BGR));
}

void cemu_pcf8833_4wire_write_byte(peripheral_t *p, soc_t *s,
                              int is_data, uint8_t value) {
    pcf8833_4wire_state_t *st = p->state;
    if (st->common.reset_asserted) return;
    if (!is_data) {
        int known;
        const char *name = begin_command(p, value, &known);
        cemu_lcd_common_emit_command(&st->common, s, st, value, known, name,
                                trace_command_extra);
        return;
    }
    if (st->ram_write) {
        cemu_lcd_common_emit_unsupported(&st->common, s, 8, 1,
                                     &PCF8833_4WIRE_OPS);
        return;
    }
    apply_parameter(p, s, value);
    cemu_lcd_common_emit_data(&st->common, s, st, value, 1,
                          st->x, st->y,
                          "PCF8833-compatible parameter",
                          trace_parameter_extra);
}

static void increment_address(pcf8833_4wire_state_t *st) {
    if (st->madctl & MADCTL_VERTICAL) {
        if (st->y >= st->end_y) {
            st->y = st->window_start_y;
            st->x = st->x >= st->end_x
                         ? st->window_start_x
                         : (uint8_t)(st->x + 1u);
        } else {
            st->y++;
        }
    } else if (st->x >= st->end_x) {
        st->x = st->window_start_x;
        st->y = st->y >= st->end_y
                     ? st->window_start_y
                     : (uint8_t)(st->y + 1u);
    } else {
        st->x++;
    }
}

/* SL55 SW20 writes the complete viewport with PASET ending one row beyond it.
 * Presentation may finish there; the controller cursor/window remain inclusive.
 * Require a contiguous horizontal sweep from the viewport origin, not a byte
 * count or CS edge. The same predicate completes held frames in older saves.
 */
static void present_complete_viewport(peripheral_t *p, soc_t *s) {
    pcf8833_4wire_state_t *st = p->state;
    lcd_common_state_t *c = &st->common;
    lcd_sweep_tracker_t *t = &st->sweep;
    unsigned next_y = c->panel_origin_y + c->panel_height;
    if (t->active && !t->pack_phase && !(t->mode & MADCTL_VERTICAL) &&
        t->origin_x == c->panel_origin_x &&
        t->origin_y == c->panel_origin_y &&
        t->start_x == t->origin_x && t->start_y == t->origin_y &&
        t->end_x + 1u == c->panel_origin_x + c->panel_width &&
        t->end_y >= next_y && t->expected_x == t->origin_x &&
        t->expected_y == next_y) {
        cemu_lcd_sweep_publish(p, s, c, t, &PCF8833_4WIRE_OPS,
                              "viewport_complete");
    }
}

void cemu_pcf8833_4wire_write_pixel(peripheral_t *p, soc_t *s, uint16_t value) {
    pcf8833_4wire_state_t *st = p->state;
    if (st->common.reset_asserted || !st->ram_write) return;
    unsigned x = st->x;
    unsigned y = st->y;
    unsigned next_x = x, next_y = y;
    if (st->madctl & MADCTL_VERTICAL) {
        if (y >= st->end_y) {
            next_y = st->window_start_y;
            next_x = x >= st->end_x ? st->window_start_x : x + 1u;
        } else next_y++;
    } else if (x >= st->end_x) {
        next_x = st->window_start_x;
        next_y = y >= st->end_y ? st->window_start_y : y + 1u;
    } else next_x++;
    lcd_sweep_write_t write = {
        .x = x, .y = y, .next_x = next_x, .next_y = next_y,
        .start_x = st->window_start_x, .start_y = st->window_start_y,
        .end_x = st->end_x, .end_y = st->end_y,
        .mode = st->madctl, .storage_writes = 1, .data_bytes = 1,
        .windowed = 1,
        .terminal = next_x == st->window_start_x &&
                    next_y == st->window_start_y,
    };
    cemu_lcd_sweep_prepare_write(p, s, &st->common, &st->sweep, &write,
                                 &PCF8833_4WIRE_OPS);
    value &= 0x0FFFu;
    if (x < PCF8833_4WIRE_WIDTH && y < PCF8833_4WIRE_HEIGHT) {
        st->gram[y * PCF8833_4WIRE_WIDTH + x] = value;
        cemu_lcd_common_note_pixel(&st->common, s);
        cemu_lcd_common_emit_data(&st->common, s, st, value, 2, x, y,
                              "PCF8833-compatible RGB444 GRAM",
                              trace_pixel_extra);
    }
    increment_address(st);
    write.next_x = st->x;
    write.next_y = st->y;
    cemu_lcd_sweep_finish_write(p, s, &st->common, &st->sweep, &write,
                                &PCF8833_4WIRE_OPS);
    present_complete_viewport(p, s);
}

static void present(peripheral_t *p) {
    pcf8833_4wire_state_t *st = p->state;
    memcpy(st->presented_gram, st->gram, sizeof st->presented_gram);
}

static void close_transaction(peripheral_t *p, soc_t *s) {
    pcf8833_4wire_state_t *st = p->state;
    cemu_lcd_sweep_close_transaction(p, s, &st->common, &st->sweep,
                                     &PCF8833_4WIRE_OPS, "pcf8833-4wire");
}

static void discard_transaction(peripheral_t *p) {
    pcf8833_4wire_state_t *st = p->state;
    cemu_lcd_sweep_discard_transaction(&st->common, &st->sweep);
}

static int ssc_supported(peripheral_t *p, unsigned frame_bits,
                         int msb_first, int is_data) {
    pcf8833_4wire_state_t *st = p->state;
    if (!msb_first) return 0;
    if (!is_data) return frame_bits == 8;
    if (st->ram_write) return frame_bits == 12 && st->colmod == 3;
    return frame_bits == 8;
}

static void ssc_consume(peripheral_t *p, soc_t *s, uint16_t tx,
                        unsigned frame_bits, int msb_first, int is_data) {
    (void)msb_first;
    if (frame_bits == 12)
        cemu_pcf8833_4wire_write_pixel(p, s, tx);
    else
        cemu_pcf8833_4wire_write_byte(p, s, is_data, (uint8_t)tx);
}

static void pcf8833_4wire_tick(peripheral_t *p, soc_t *s, int n) {
    pcf8833_4wire_state_t *st = p->state;
    present_complete_viewport(p, s);
    cemu_lcd_common_tick(p, s, n, &st->common, &PCF8833_4WIRE_OPS);
}

static uint64_t pcf8833_4wire_next_event(peripheral_t *p, soc_t *s) {
    pcf8833_4wire_state_t *st = p->state;
    return cemu_lcd_common_next_event(p, s, &st->common,
                                 &PCF8833_4WIRE_OPS);
}

void cemu_pcf8833_4wire_ssc_start(void *ctx, soc_t *s, uint16_t tx,
                             unsigned frame_bits, int msb_first) {
    peripheral_t *p = ctx;
    pcf8833_4wire_state_t *st = p->state;
    cemu_lcd_common_ssc_start(p, s, tx, frame_bits, msb_first,
                         &st->common, &PCF8833_4WIRE_OPS);
}

uint16_t cemu_pcf8833_4wire_ssc_complete(void *ctx, soc_t *s, uint16_t tx,
                                    unsigned frame_bits, int msb_first) {
    peripheral_t *p = ctx;
    pcf8833_4wire_state_t *st = p->state;
    return cemu_lcd_common_ssc_complete(p, s, tx, frame_bits, msb_first,
                                   &st->common, &PCF8833_4WIRE_OPS);
}

void cemu_pcf8833_4wire_ssc_abort(void *ctx, soc_t *s) {
    (void)s;
    peripheral_t *p = ctx;
    pcf8833_4wire_state_t *st = p->state;
    cemu_lcd_common_ssc_abort(&st->common);
}

int cemu_pcf8833_4wire_flush_pending_frame(peripheral_t *p, soc_t *s) {
    pcf8833_4wire_state_t *st = p->state;
    return cemu_lcd_common_flush_pending_frame(p, s, &st->common,
                                          &PCF8833_4WIRE_OPS);
}

static void pixel_rgb(const void *state, unsigned x, unsigned y,
                      int raw, uint8_t out[3]) {
    const pcf8833_4wire_state_t *st = state;
    unsigned source_x = x;
    unsigned source_y = y;
    if (!raw) {
        if (st->common.panel_mirror_x)
            source_x = st->common.panel_width - 1u - source_x;
        if (st->madctl & MADCTL_MX)
            source_x = st->common.panel_width - 1u - source_x;
        if (st->common.panel_mirror_y)
            source_y = st->common.panel_height - 1u - source_y;
        if (st->madctl & MADCTL_MY)
            source_y = st->common.panel_height - 1u - source_y;
        source_x += st->common.panel_origin_x;
        source_y += st->common.panel_origin_y;
    }

    const uint16_t *memory = raw ? st->gram : st->presented_gram;
    uint16_t pixel =
        source_x < PCF8833_4WIRE_WIDTH && source_y < PCF8833_4WIRE_HEIGHT
      ? memory[source_y * PCF8833_4WIRE_WIDTH + source_x] : 0;
    unsigned r = (pixel >> 8) & 0xFu;
    unsigned g = (pixel >> 4) & 0xFu;
    unsigned b = pixel & 0xFu;
    if (!raw) {
        if (st->common.reset_asserted || !st->sleep_out ||
            !st->display_on || st->all_pixels == 2) {
            r = g = b = 0;
        } else if (st->all_pixels == 1) {
            r = g = b = 15;
        } else if (st->idle_mode) {
            r = r & 8u ? 15u : 0u;
            g = g & 8u ? 15u : 0u;
            b = b & 8u ? 15u : 0u;
        }
        if (st->reverse) {
            r = 15u - r;
            g = 15u - g;
            b = 15u - b;
        }
    }
    /* MADCTL.BGR is recorded but not applied: measured wire order is RGB. */
    out[0] = (uint8_t)(r * 17u);
    out[1] = (uint8_t)(g * 17u);
    out[2] = (uint8_t)(b * 17u);
}

int cemu_pcf8833_4wire_render_rgb(const pcf8833_4wire_state_t *st, int raw,
                             unsigned scale, uint8_t *rgb, size_t rgb_len) {
    return st ? cemu_lcd_common_render_rgb(&st->common, st, raw,
                                       PCF8833_4WIRE_WIDTH,
                                       PCF8833_4WIRE_HEIGHT, scale,
                                       rgb, rgb_len, pixel_rgb) : -1;
}

void cemu_pcf8833_4wire_periph_init(peripheral_t *p,
                               pcf8833_4wire_state_t *st,
                               const device_lcd_config_t *cfg) {
    memset(p, 0, sizeof *p);
    memset(st, 0, sizeof *st);
    cemu_lcd_common_init(&st->common, cfg,
                     PCF8833_4WIRE_WIDTH, PCF8833_4WIRE_HEIGHT);
    reset_registers(st);

    p->id = "lcd";
    p->model = "pcf8833-4wire";
    p->state = st;
    p->tick = pcf8833_4wire_tick;
    p->next_event_ticks = pcf8833_4wire_next_event;
    p->advance_quiet = cemu_lcd_common_advance_quiet;
}

static const lcd_controller_ops_t PCF8833_4WIRE_OPS = {
    .select_detail = "PCF8833-compatible CS",
    .reset_detail = "PCF8833-compatible RES",
    .frame_detail = "PCF8833-compatible RGB444 GRAM frame",
    .unsupported_detail = "PCF8833-compatible unsupported serial transfer",
    .data_active_high = 1,
    .select_reports_reset = 1,
    .track_reset = 1,
    .reset = cemu_pcf8833_4wire_reset,
    .write_byte = cemu_pcf8833_4wire_write_byte,
    .ssc_supported = ssc_supported,
    .ssc_consume = ssc_consume,
    .close_transaction = close_transaction,
    .discard_transaction = discard_transaction,
    .present = present,
};
