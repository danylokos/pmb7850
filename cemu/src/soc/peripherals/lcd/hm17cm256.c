/* Hynix HM17CM256: command banks, RGB332 GRAM, and serial attachment. */
#include <string.h>

#include "hm17cm256.h"
#include "soc.h"

static const lcd_controller_ops_t HM17CM256_OPS;

static void clear_transaction(hm17cm256_state_t *st) {
    cemu_lcd_sweep_reset(&st->common, &st->sweep);
    st->transaction_started = 0;
    st->transaction_sweep_member = 0;
    st->sweep_active = 0;
    st->sweep_data_bytes = 0;
}

static void discard_transaction(peripheral_t *p) {
    clear_transaction(p->state);
}

static void reset_registers(hm17cm256_state_t *st) {
    static const uint8_t DEFAULT_PALETTE[8] = {
        0, 5, 10, 14, 17, 21, 26, 31
    };
    st->hm17.x = st->hm17.y = 0;
    st->hm17.window_start_x = st->hm17.window_start_y = 0;
    st->hm17.end_x = HM17CM256_WIDTH - 1u;
    st->hm17.end_y = HM17CM256_HEIGHT - 1u;
    st->hm17.start_line = st->hm17.n_line = 0;
    st->hm17.line_start = 0;
    st->hm17.line_end = HM17CM256_HEIGHT - 1u;
    st->hm17.re = st->hm17.scan_start = 0;
    st->hm17.duty = st->hm17.boost = st->hm17.bias = 0;
    st->hm17.electric_volume = 0;
    st->hm17.display_on = st->hm17.all_on = st->hm17.reverse = 0;
    st->hm17.common_shift = st->hm17.n_line_enable = 0;
    st->hm17.swap = st->hm17.reflect = 0;
    st->hm17.window = st->hm17.aim = 0;
    st->hm17.increment_x = st->hm17.increment_y = 0;
    st->hm17.amp_on = st->hm17.halt = st->hm17.dc_on = 0;
    st->hm17.discharge = st->hm17.monochrome = 0;
    st->hm17.fixed_pwm = st->hm17.clock_select = 0;
    st->hm17.word_length_16 = 0;
    st->glsb = st->extension_cs = 0;
    st->oscillator_rf = st->oscillator_ffl = 0;
    st->dummy_y = st->pwm_mode_control = 0;
    cemu_lcd_common_reset_serial(&st->common);
    clear_transaction(st);
    for (unsigned c = 0; c < 3; c++)
        memcpy(st->palette[c], DEFAULT_PALETTE, sizeof DEFAULT_PALETTE);
}

void cemu_hm17cm256_reset(peripheral_t *p, soc_t *s) {
    hm17cm256_state_t *st = p->state;
    reset_registers(st);
    cemu_lcd_common_emit_reset(&st->common, s, 1, &HM17CM256_OPS);
}

static uint8_t reverse_bits(uint8_t value) {
    value = (uint8_t)(((value & 0x55u) << 1) | ((value >> 1) & 0x55u));
    value = (uint8_t)(((value & 0x33u) << 2) | ((value >> 2) & 0x33u));
    return (uint8_t)((value << 4) | (value >> 4));
}

static void trace_command_extra(cemu_native_trace_event_t *ev, const void *state) {
    const hm17cm256_state_t *st = state;
    cemu_event_field_i64(&ev->info, "bank", st->hm17.re);
    cemu_event_field_i64(&ev->info, "x", st->hm17.x);
    cemu_event_field_i64(&ev->info, "y", st->hm17.y);
    cemu_event_field_bool(&ev->info, "reset", st->common.reset_asserted);
}

static void trace_data_extra(cemu_native_trace_event_t *ev, const void *state) {
    const hm17cm256_state_t *st = state;
    cemu_event_field_i64(&ev->info, "bank", st->hm17.re);
    cemu_event_field_bool(&ev->info, "swap", st->hm17.swap);
    cemu_event_field_bool(&ev->info, "ref", st->hm17.reflect);
    cemu_event_field_bool(&ev->info, "glsb", st->glsb);
    cemu_event_field_bool(&ev->info, "fixed_pwm", st->hm17.fixed_pwm);
    cemu_event_field_bool(&ev->info, "wls", st->hm17.word_length_16);
}

static void write_gram_byte(peripheral_t *p, soc_t *s, uint8_t value) {
    hm17cm256_state_t *st = p->state;
    unsigned logical_x = st->hm17.x;
    unsigned y = st->hm17.y;
    unsigned physical_x = st->hm17.reflect
                        ? HM17CM256_WIDTH - 1u - logical_x : logical_x;
    uint8_t pixel = st->hm17.reflect != st->hm17.swap
                  ? reverse_bits(value) : value;

    unsigned end_x = st->hm17.window ? st->hm17.end_x
                   : st->sweep.active ? st->sweep.end_x
                                      : HM17CM256_WIDTH - 1u;
    unsigned end_y = st->hm17.window ? st->hm17.end_y
                   : st->common.panel_origin_y + st->common.panel_height - 1u;
    hm17_state_t next = st->hm17;
    cemu_hm17_increment_address(&next, HM17CM256_WIDTH - 1u,
                                HM17CM256_HEIGHT - 1u);
    int terminal = st->hm17.window
        ? next.x == st->hm17.window_start_x &&
          next.y == st->hm17.window_start_y
        : st->sweep.active && logical_x == end_x && y == end_y;
    lcd_sweep_write_t write = {
        .x = logical_x, .y = y, .next_x = next.x, .next_y = next.y,
        .start_x = st->hm17.window ? st->hm17.window_start_x
                                   : st->sweep.active ? st->sweep.start_x
                                                      : logical_x,
        .start_y = st->hm17.window ? st->hm17.window_start_y
                                   : st->common.panel_origin_y,
        .end_x = end_x, .end_y = end_y,
        .mode = st->hm17.increment_x | (st->hm17.increment_y << 1) |
                (st->hm17.window << 2) | (st->hm17.reflect << 3) |
                (st->hm17.swap << 4) | (st->glsb << 5) |
                (st->hm17.fixed_pwm << 6),
        .storage_writes = physical_x < HM17CM256_WIDTH &&
                          y < HM17CM256_HEIGHT,
        .data_bytes = 1, .windowed = st->hm17.window,
        .terminal = terminal,
    };
    cemu_lcd_sweep_prepare_write(p, s, &st->common,
                                 &st->sweep, &write, &HM17CM256_OPS);
    if (physical_x < HM17CM256_WIDTH && y < HM17CM256_HEIGHT) {
        st->gram[y * HM17CM256_WIDTH + physical_x] = pixel;
        cemu_lcd_common_note_pixel(&st->common, s);
        cemu_lcd_common_emit_data(&st->common, s, st, pixel, 1,
                              physical_x, y, "HM17CM256 GRAM",
                              trace_data_extra);
    }
    cemu_hm17_increment_address(&st->hm17,
                                  HM17CM256_WIDTH - 1u,
                                  HM17CM256_HEIGHT - 1u);
    write.next_x = st->hm17.x;
    write.next_y = st->hm17.y;
    cemu_lcd_sweep_finish_write(p, s, &st->common,
                                &st->sweep, &write, &HM17CM256_OPS);
}

static void present(peripheral_t *p) {
    hm17cm256_state_t *st = p->state;
    memcpy(st->presented_gram, st->gram, sizeof st->presented_gram);
}

static void close_transaction(peripheral_t *p, soc_t *s) {
    hm17cm256_state_t *st = p->state;
    /* Unwindowed HM traffic commonly programs one row per CS.  The first
     * completed row establishes its X span; following rows must restart at
     * that X and advance Y through the configured viewport. */
    if (st->sweep.active && !st->sweep.windowed &&
        st->hm17.increment_x && !st->hm17.increment_y &&
        st->hm17.y == st->sweep.transaction_y &&
        st->hm17.x > st->sweep.transaction_x) {
        uint16_t row_end = (uint16_t)(st->hm17.x - 1u);
        if (st->sweep.transaction_y == st->sweep.origin_y) {
            st->sweep.start_x = st->sweep.origin_x;
            st->sweep.end_x = row_end;
        } else if (row_end != st->sweep.end_x) {
            cemu_lcd_sweep_fail_open(p, s, &st->common, &st->sweep,
                                     &HM17CM256_OPS);
        }
        st->sweep.expected_x = st->sweep.origin_x;
        st->sweep.expected_y = (uint16_t)(st->sweep.transaction_y + 1u);
    }
    cemu_lcd_sweep_close_transaction(p, s, &st->common, &st->sweep,
                                     &HM17CM256_OPS, "hm17cm256");
    st->transaction_count = st->sweep.transaction_sequence;
}

static void select_changed(peripheral_t *p, soc_t *s, int selected) {
    (void)s;
    if (!selected) return;
    hm17cm256_state_t *st = p->state;
    st->transaction_started = 0;
    st->transaction_sweep_member = 0;
}

static int palette_target(uint8_t re, uint8_t address,
                          unsigned *channel, unsigned *index, int *upper) {
    if (re == 1 && address <= 0x0D) {
        *channel = 0; *index = address >> 1; *upper = address & 1; return 1;
    }
    if (re == 2) {
        if (address <= 0x01) {
            *channel = 0; *index = 7; *upper = address & 1; return 1;
        }
        if (address <= 0x0D) {
            *channel = 1; *index = (address - 2u) >> 1;
            *upper = address & 1; return 1;
        }
    }
    if (re == 3) {
        if (address <= 0x03) {
            *channel = 1; *index = 6u + (address >> 1);
            *upper = address & 1; return 1;
        }
        if (address <= 0x0D) {
            *channel = 2; *index = (address - 4u) >> 1;
            *upper = address & 1; return 1;
        }
    }
    if (re == 4 && address <= 0x05) {
        *channel = 2; *index = 5u + (address >> 1);
        *upper = address & 1; return 1;
    }
    return 0;
}

static const char *decode_command(peripheral_t *p, soc_t *s,
                                  uint8_t value, int *known) {
    hm17cm256_state_t *st = p->state;
    uint8_t address = value & 0x0Fu;
    *known = 1;
    if ((value & 0xF8u) == 0xF0u) {
        st->hm17.re = value & 7u;
        return "re_register";
    }

    unsigned channel, index;
    int upper;
    if (palette_target(st->hm17.re, value >> 4,
                       &channel, &index, &upper)) {
        if (upper)
            st->palette[channel][index] =
                (uint8_t)((st->palette[channel][index] & 0x0Fu) |
                          ((value & 1u) << 4));
        else
            st->palette[channel][index] =
                (uint8_t)((st->palette[channel][index] & 0x10u) |
                          address);
        return "gradation_palette";
    }

    if (st->hm17.re == 0) {
        switch (value >> 4) {
        case 0x0:
            st->hm17.x = (uint8_t)((st->hm17.x & 0xF0u) | address);
            st->hm17.window_start_x = st->hm17.x;
            return "x_address_lower";
        case 0x1:
            st->hm17.x = (uint8_t)((st->hm17.x & 0x0Fu) | (address << 4));
            st->hm17.window_start_x = st->hm17.x;
            return "x_address_upper";
        case 0x2:
            st->hm17.y = (uint8_t)((st->hm17.y & 0xF0u) | address);
            st->hm17.window_start_y = st->hm17.y;
            return "y_address_lower";
        case 0x3:
            st->hm17.y = (uint8_t)((st->hm17.y & 0x0Fu) | (address << 4));
            st->hm17.window_start_y = st->hm17.y;
            return "y_address_upper";
        case 0x4:
            st->hm17.start_line =
                (uint8_t)((st->hm17.start_line & 0xF0u) | address);
            return "start_line_lower";
        case 0x5:
            st->hm17.start_line =
                (uint8_t)((st->hm17.start_line & 0x0Fu) | (address << 4));
            return "start_line_upper";
        case 0x6:
            st->hm17.n_line =
                (uint8_t)((st->hm17.n_line & 0xF0u) | address);
            return "n_line_lower";
        case 0x7:
            st->hm17.n_line =
                (uint8_t)((st->hm17.n_line & 0x0Fu) | (address << 4));
            return "n_line_upper";
        case 0x8:
            st->hm17.common_shift = (value >> 3) & 1u;
            st->hm17.monochrome = (value >> 2) & 1u;
            st->hm17.all_on = (value >> 1) & 1u;
            st->hm17.display_on = value & 1u;
            return "display_control_1";
        case 0x9:
            st->hm17.reverse = (value >> 3) & 1u;
            st->hm17.n_line_enable = (value >> 2) & 1u;
            st->hm17.swap = (value >> 1) & 1u;
            st->hm17.reflect = value & 1u;
            return "display_control_2";
        case 0xA:
            st->hm17.window = (value >> 3) & 1u;
            st->hm17.aim = (value >> 2) & 1u;
            st->hm17.increment_y = (value >> 1) & 1u;
            st->hm17.increment_x = value & 1u;
            return "increment_control";
        case 0xB:
            st->hm17.amp_on = (value >> 3) & 1u;
            st->hm17.halt = (value >> 2) & 1u;
            st->hm17.dc_on = (value >> 1) & 1u;
            if (value & 1u) cemu_hm17cm256_reset(p, s);
            return (value & 1u) ? "all_clear" : "power_control";
        case 0xC: st->hm17.duty = address; return "lcd_duty";
        case 0xD: st->hm17.boost = value & 7u; return "boost";
        case 0xE: st->hm17.bias = value & 7u; return "bias";
        default: break;
        }
    } else if (st->hm17.re == 4) {
        switch (value >> 4) {
        case 0x6: st->hm17.scan_start = address; return "display_scan_start";
        case 0x7: st->extension_cs = address; return "extension_chip_select";
        case 0x8:
            st->hm17.fixed_pwm = (value >> 3) & 1u;
            st->glsb = (value >> 2) & 1u;
            return "display_selection";
        case 0x9:
            st->hm17.clock_select = (value >> 1) & 1u;
            st->hm17.word_length_16 = value & 1u;
            return "ram_data_length";
        case 0xA:
            st->hm17.electric_volume =
                (uint8_t)((st->hm17.electric_volume & 0x70u) | address);
            return "electric_volume_lower";
        case 0xB:
            st->hm17.electric_volume =
                (uint8_t)((st->hm17.electric_volume & 0x0Fu) |
                          ((value & 7u) << 4));
            return "electric_volume_upper";
        case 0xC: return "register_read_address";
        case 0xD:
            st->oscillator_rf = (value >> 2) & 3u;
            st->oscillator_ffl = value & 3u;
            return "oscillator_control";
        case 0xE: st->hm17.discharge = value & 1u; return "discharge";
        default: break;
        }
    } else if (st->hm17.re == 5) {
        switch (value >> 4) {
        case 0x0:
            st->hm17.end_x = (uint8_t)((st->hm17.end_x & 0xF0u) | address);
            return "window_end_x_lower";
        case 0x1:
            st->hm17.end_x =
                (uint8_t)((st->hm17.end_x & 0x0Fu) | (address << 4));
            return "window_end_x_upper";
        case 0x2:
            st->hm17.end_y = (uint8_t)((st->hm17.end_y & 0xF0u) | address);
            return "window_end_y_lower";
        case 0x3:
            st->hm17.end_y =
                (uint8_t)((st->hm17.end_y & 0x0Fu) | (address << 4));
            return "window_end_y_upper";
        case 0x4:
            st->hm17.line_start =
                (uint8_t)((st->hm17.line_start & 0xF0u) | address);
            return "line_start_lower";
        case 0x5:
            st->hm17.line_start =
                (uint8_t)((st->hm17.line_start & 0x0Fu) | (address << 4));
            return "line_start_upper";
        case 0x6:
            st->hm17.line_end =
                (uint8_t)((st->hm17.line_end & 0xF0u) | address);
            return "line_end_lower";
        case 0x7:
            st->hm17.line_end =
                (uint8_t)((st->hm17.line_end & 0x0Fu) | (address << 4));
            return "line_end_upper";
        case 0x8: return "line_inversion_control";
        case 0x9: st->dummy_y = address; return "dummy_y_select";
        case 0xA:
            st->pwm_mode_control = address;
            return "four_bit_pwm_control";
        default: break;
        }
    }

    *known = 0;
    return "unknown";
}

void cemu_hm17cm256_write_byte(peripheral_t *p, soc_t *s,
                          int is_data, uint8_t value) {
    hm17cm256_state_t *st = p->state;
    if (st->common.reset_asserted) return;
    if (is_data) {
        if (st->hm17.word_length_16) {
            cemu_lcd_common_emit_unsupported(&st->common, s, 8, 1,
                                         &HM17CM256_OPS);
            return;
        }
        write_gram_byte(p, s, value);
        return;
    }
    unsigned group = value >> 4;
    uint8_t nibble = value & 0x0Fu;
    int addressing_mutation = 0;
    if (st->hm17.re == 0 && group == 0x9)
        addressing_mutation = st->hm17.swap != ((value >> 1) & 1u) ||
                              st->hm17.reflect != (value & 1u);
    else if (st->hm17.re == 0 && group == 0xA)
        addressing_mutation = st->hm17.window != ((value >> 3) & 1u) ||
            st->hm17.increment_y != ((value >> 1) & 1u) ||
            st->hm17.increment_x != (value & 1u);
    else if (st->hm17.re == 4 && group == 0x8)
        addressing_mutation = st->hm17.fixed_pwm != ((value >> 3) & 1u) ||
                              st->glsb != ((value >> 2) & 1u);
    else if (st->hm17.re == 4 && group == 0x9)
        addressing_mutation = st->hm17.word_length_16 != (value & 1u);
    else if (st->hm17.re == 5 && group == 0x0)
        addressing_mutation = ((st->hm17.end_x & 0xF0u) | nibble) !=
                              st->hm17.end_x;
    else if (st->hm17.re == 5 && group == 0x1)
        addressing_mutation = ((st->hm17.end_x & 0x0Fu) | (nibble << 4)) !=
                              st->hm17.end_x;
    else if (st->hm17.re == 5 && group == 0x2)
        addressing_mutation = ((st->hm17.end_y & 0xF0u) | nibble) !=
                              st->hm17.end_y;
    else if (st->hm17.re == 5 && group == 0x3)
        addressing_mutation = ((st->hm17.end_y & 0x0Fu) | (nibble << 4)) !=
                              st->hm17.end_y;
    if (addressing_mutation)
        cemu_lcd_sweep_fail_open(p, s, &st->common, &st->sweep,
                                 &HM17CM256_OPS);
    int known;
    const char *name = decode_command(p, s, value, &known);
    cemu_lcd_common_emit_command(&st->common, s, st, value, known, name,
                            trace_command_extra);
}

static int ssc_supported(peripheral_t *p, unsigned frame_bits,
                         int msb_first, int is_data) {
    hm17cm256_state_t *st = p->state;
    return msb_first && frame_bits == 8 &&
           (!is_data || !st->hm17.word_length_16);
}

static void ssc_consume(peripheral_t *p, soc_t *s, uint16_t tx,
                        unsigned frame_bits, int msb_first, int is_data) {
    (void)msb_first;
    (void)frame_bits;
    cemu_hm17cm256_write_byte(p, s, is_data, (uint8_t)tx);
}

static void hm17cm256_tick(peripheral_t *p, soc_t *s, int n) {
    hm17cm256_state_t *st = p->state;
    cemu_lcd_common_tick(p, s, n, &st->common, &HM17CM256_OPS);
}

static uint64_t hm17cm256_next_event(peripheral_t *p, soc_t *s) {
    hm17cm256_state_t *st = p->state;
    return cemu_lcd_common_next_event(p, s, &st->common, &HM17CM256_OPS);
}

void cemu_hm17cm256_ssc_start(void *ctx, soc_t *s, uint16_t tx,
                         unsigned frame_bits, int msb_first) {
    peripheral_t *p = ctx;
    hm17cm256_state_t *st = p->state;
    cemu_lcd_common_ssc_start(p, s, tx, frame_bits, msb_first,
                         &st->common, &HM17CM256_OPS);
}

uint16_t cemu_hm17cm256_ssc_complete(void *ctx, soc_t *s, uint16_t tx,
                                unsigned frame_bits, int msb_first) {
    peripheral_t *p = ctx;
    hm17cm256_state_t *st = p->state;
    return cemu_lcd_common_ssc_complete(p, s, tx, frame_bits, msb_first,
                                   &st->common, &HM17CM256_OPS);
}

void cemu_hm17cm256_ssc_abort(void *ctx, soc_t *s) {
    (void)s;
    peripheral_t *p = ctx;
    hm17cm256_state_t *st = p->state;
    cemu_lcd_common_ssc_abort(&st->common);
}

int cemu_hm17cm256_flush_pending_frame(peripheral_t *p, soc_t *s) {
    hm17cm256_state_t *st = p->state;
    return cemu_lcd_common_flush_pending_frame(p, s, &st->common,
                                          &HM17CM256_OPS);
}

static unsigned blue_index(const hm17cm256_state_t *st, unsigned value) {
    value &= 3u;
    if (!st->hm17.fixed_pwm) return (value << 1) | st->glsb;
    if (value == 0) return 0;
    if (value == 3) return 7;
    return (value << 1) | st->glsb;
}

static uint8_t scaled_level(unsigned value, unsigned maximum) {
    return (uint8_t)((value * 255u + maximum / 2u) / maximum);
}

static void pixel_rgb(const void *state, unsigned x, unsigned y,
                      int raw, uint8_t out[3]) {
    const hm17cm256_state_t *st = state;
    unsigned source_x = x, source_y = y;
    if (!raw) {
        if (st->common.panel_mirror_x) x = st->common.panel_width - 1u - x;
        if (st->common.panel_mirror_y) y = st->common.panel_height - 1u - y;
        source_x = st->common.panel_origin_x + x;
        unsigned line = st->common.panel_origin_y + y;
        if (st->hm17.common_shift)
            line = HM17CM256_GRAPHIC_HEIGHT - 1u - line;
        source_y = (st->hm17.start_line +
                    15u * (st->hm17.scan_start & 0x0Fu) + line) %
                   HM17CM256_GRAPHIC_HEIGHT;
    }
    const uint8_t *memory = raw ? st->gram : st->presented_gram;
    uint8_t pixel = (source_x < HM17CM256_WIDTH &&
                     source_y < HM17CM256_HEIGHT)
                  ? memory[source_y * HM17CM256_WIDTH + source_x] : 0;
    unsigned a = (pixel >> 5) & 7u;
    unsigned b = (pixel >> 2) & 7u;
    unsigned c = pixel & 3u;
    if (raw) {
        out[0] = scaled_level(a, 7);
        out[1] = scaled_level(b, 7);
        out[2] = scaled_level(c, 3);
        return;
    }
    if (st->common.reset_asserted || !st->hm17.display_on ||
        st->hm17.halt) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    if (st->hm17.all_on) {
        out[0] = out[1] = out[2] = 255;
        return;
    }
    if (st->hm17.monochrome) {
        uint8_t level = pixel ? 255u : 0u;
        out[0] = out[1] = out[2] = level;
    } else if (st->hm17.fixed_pwm) {
        out[0] = scaled_level(a, 7);
        out[1] = scaled_level(b, 7);
        out[2] = scaled_level(blue_index(st, c), 7);
    } else {
        out[0] = scaled_level(st->palette[0][a], 31);
        out[1] = scaled_level(st->palette[1][b], 31);
        out[2] = scaled_level(st->palette[2][blue_index(st, c)], 31);
    }
    if (st->hm17.reverse) {
        out[0] = (uint8_t)(255u - out[0]);
        out[1] = (uint8_t)(255u - out[1]);
        out[2] = (uint8_t)(255u - out[2]);
    }
}

int cemu_hm17cm256_render_rgb(const hm17cm256_state_t *st, int raw,
                         unsigned scale, uint8_t *rgb, size_t rgb_len) {
    return st ? cemu_lcd_common_render_rgb(&st->common, st, raw,
                                       HM17CM256_WIDTH, HM17CM256_HEIGHT,
                                       scale, rgb, rgb_len, pixel_rgb) : -1;
}

void cemu_hm17cm256_periph_init(peripheral_t *p, hm17cm256_state_t *st,
                           const device_lcd_config_t *cfg) {
    memset(p, 0, sizeof *p);
    memset(st, 0, sizeof *st);
    cemu_lcd_common_init(&st->common, cfg,
                     HM17CM256_WIDTH, HM17CM256_HEIGHT);
    reset_registers(st);
    p->id = "lcd";
    p->model = "hm17cm256";
    p->state = st;
    p->tick = hm17cm256_tick;
    p->next_event_ticks = hm17cm256_next_event;
    p->advance_quiet = cemu_lcd_common_advance_quiet;
}

static const lcd_controller_ops_t HM17CM256_OPS = {
    .select_detail = "HM17CM256 CS",
    .reset_detail = "HM17CM256 RES",
    .frame_detail = "HM17CM256 GRAM frame",
    .unsupported_detail = "HM17CM256 unsupported serial transfer",
    .select_reports_reset = 1,
    .track_reset = 1,
    .reset = cemu_hm17cm256_reset,
    .write_byte = cemu_hm17cm256_write_byte,
    .ssc_supported = ssc_supported,
    .ssc_consume = ssc_consume,
    .select_changed = select_changed,
    .close_transaction = close_transaction,
    .discard_transaction = discard_transaction,
    .present = present,
};
