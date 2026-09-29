/* Hynix HM17CM4096: command banks, RGB444 GRAM, and serial board attachment. */
#include <string.h>

#include "hm17cm4096.h"
#include "soc.h"

static const lcd_controller_ops_t HM17CM4096_OPS;

static void reset_registers(hm17cm4096_state_t *st) {
    static const uint8_t DEFAULT_PALETTE[16] = {
        0, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31
    };
    st->hm17.x = st->hm17.y = st->hm17.window_start_x = st->hm17.window_start_y = 0;
    st->hm17.end_x = 0xFF;
    st->hm17.end_y = HM17CM4096_HEIGHT - 1u;
    st->hm17.start_line = st->hm17.n_line = 0;
    st->hm17.line_start = 0;
    st->hm17.line_end = HM17CM4096_HEIGHT - 1u;
    st->hm17.re = st->hm17.scan_start = st->hm17.duty = st->hm17.boost = st->hm17.bias = 0;
    st->hm17.electric_volume = 0;
    st->hm17.display_on = st->hm17.all_on = st->hm17.reverse = st->hm17.common_shift = 0;
    st->hm17.n_line_enable = st->hm17.swap = st->hm17.reflect = 0;
    st->hm17.window = st->hm17.aim = st->hm17.increment_x = st->hm17.increment_y = 0;
    st->hm17.amp_on = st->hm17.halt = st->hm17.dc_on = st->hm17.discharge = 0;
    st->hm17.monochrome = st->hm17.fixed_pwm = st->color_256 = 0;
    st->high_speed_write = st->absolute_12bit = st->hm17.clock_select = 0;
    st->hm17.word_length_16 = 0;
    st->palette_upper = st->line_reverse = st->blink_type = 0;
    st->pwm_control = 0;
    memset(st->pack, 0, sizeof st->pack);
    st->pack_count = 0;
    st->common.gpio_shift = st->common.ssc_shift = 0;
    st->common.gpio_shift_bits = st->common.ssc_shift_bits = 0;
    st->common.frame_pending = 0;
    cemu_lcd_sweep_reset(&st->common, &st->sweep);
    for (unsigned c = 0; c < 3; c++)
        memcpy(st->palette[c], DEFAULT_PALETTE, sizeof DEFAULT_PALETTE);
}

void cemu_hm17cm4096_reset(peripheral_t *p, soc_t *s) {
    hm17cm4096_state_t *st = (hm17cm4096_state_t *)p->state;
    reset_registers(st);
    cemu_lcd_common_emit_reset(&st->common, s, 1, &HM17CM4096_OPS);
}

static void trace_command_extra(cemu_native_trace_event_t *ev, const void *state) {
    const hm17cm4096_state_t *st = state;
    cemu_event_field_i64(&ev->info, "bank", st->hm17.re);
    cemu_event_field_i64(&ev->info, "x", st->hm17.x);
    cemu_event_field_i64(&ev->info, "y", st->hm17.y);
    cemu_event_field_bool(&ev->info, "reset", st->common.reset_asserted);
}

static void trace_data_extra(cemu_native_trace_event_t *ev, const void *state) {
    const hm17cm4096_state_t *st = state;
    cemu_event_field_i64(&ev->info, "bank", st->hm17.re);
    cemu_event_field_bool(&ev->info, "swap", st->hm17.swap);
    cemu_event_field_bool(&ev->info, "ref", st->hm17.reflect);
    cemu_event_field_bool(&ev->info, "high_speed", st->high_speed_write);
    cemu_event_field_bool(&ev->info, "color_256", st->color_256);
    cemu_event_field_bool(&ev->info, "abs", st->absolute_12bit);
    cemu_event_field_bool(&ev->info, "wls", st->hm17.word_length_16);
}

static void set_pixel(hm17cm4096_state_t *st, soc_t *s,
                      unsigned x, unsigned y, uint16_t pixel) {
    if (x >= HM17CM4096_WIDTH || y >= HM17CM4096_HEIGHT) return;
    if (st->hm17.reflect) x = HM17CM4096_WIDTH - 1u - x;
    pixel &= 0x0FFFu;
    st->gram[y * HM17CM4096_WIDTH + x] = pixel;
    cemu_lcd_common_note_pixel(&st->common, s);
    cemu_lcd_common_emit_data(&st->common, s, st, pixel, 2, x, y,
                          "HM17CM4096 GRAM",
                          trace_data_extra);
}

static uint16_t rgb444(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r & 0xFu) << 8) | ((g & 0xFu) << 4) | (b & 0xFu));
}

static void write_gram_byte(peripheral_t *p, soc_t *s, uint8_t value) {
    hm17cm4096_state_t *st = p->state;
    /*
     * The observed M55 WLS=0 path clocks each MSB-first serial octet into the
     * RGB packer unchanged. REF selects the GRAM segment direction in
     * set_pixel(); neither REF nor the board-level panel mirror changes serial
     * bit order. WLS=1 and SWAP=1 RAM-write semantics remain unproven.
     */
    uint64_t before_writes = st->common.data_seq;
    unsigned x = st->hm17.x, y = st->hm17.y;
    hm17_state_t next = st->hm17;
    cemu_hm17_increment_address(&next, 0xFFu,
                                HM17CM4096_HEIGHT - 1u);
    unsigned bytes_per_pixel = st->color_256 ? 1u
                             : st->high_speed_write ? 0u : 2u;
    unsigned start_x = st->hm17.window ? st->hm17.window_start_x
                     : bytes_per_pixel
                       ? st->common.panel_origin_x * bytes_per_pixel : x;
    unsigned end_x = st->hm17.window ? st->hm17.end_x
                   : bytes_per_pixel
                     ? (st->common.panel_origin_x + st->common.panel_width) *
                       bytes_per_pixel - 1u : 0xFFu;
    unsigned start_y = st->hm17.window ? st->hm17.window_start_y
                     : st->common.panel_origin_y;
    unsigned end_y = st->hm17.window ? st->hm17.end_y
                   : st->common.panel_origin_y + st->common.panel_height - 1u;
    lcd_sweep_write_t write = {
        .x = x, .y = y, .next_x = next.x, .next_y = next.y,
        .start_x = start_x, .start_y = start_y,
        .end_x = end_x, .end_y = end_y,
        .mode = st->hm17.increment_x | (st->hm17.increment_y << 1) |
                (st->hm17.window << 2) | (st->hm17.reflect << 3) |
                (st->hm17.swap << 4) | (st->color_256 << 5) |
                (st->high_speed_write << 6) | (st->absolute_12bit << 7),
        .pack_phase = st->pack_count, .data_bytes = 1,
        .windowed = st->hm17.window,
    };
    cemu_lcd_sweep_prepare_write(p, s, &st->common, &st->sweep, &write,
                                 &HM17CM4096_OPS);

    if (st->color_256) {
        unsigned r = (((value >> 5) & 7u) << 1) | 1u;
        unsigned g = (((value >> 2) & 7u) << 1) | 1u;
        unsigned b = ((value & 3u) << 2) | 3u;
        set_pixel(st, s, st->hm17.x, st->hm17.y, rgb444(r, g, b));
    } else if (st->high_speed_write) {
        st->pack[st->pack_count++] = value;
        if (st->pack_count == 3) {
            unsigned group = st->hm17.x / 3u;
            set_pixel(st, s, group * 2u, st->hm17.y,
                      rgb444(st->pack[0] >> 4, st->pack[0],
                             st->pack[1] >> 4));
            set_pixel(st, s, group * 2u + 1u, st->hm17.y,
                      rgb444(st->pack[1], st->pack[2] >> 4,
                             st->pack[2]));
            st->pack_count = 0;
        }
    } else {
        st->pack[st->pack_count++] = value;
        if (st->pack_count == 2) {
            uint16_t pixel;
            if (st->absolute_12bit) {
                pixel = rgb444(st->pack[0], st->pack[1] >> 4, st->pack[1]);
            } else {
                unsigned r = st->pack[0] >> 4;
                unsigned g = ((st->pack[0] & 7u) << 1) |
                             ((st->pack[1] >> 7) & 1u);
                unsigned b = (st->pack[1] >> 1) & 0xFu;
                pixel = rgb444(r, g, b);
            }
            set_pixel(st, s, st->hm17.x / 2u, st->hm17.y, pixel);
            st->pack_count = 0;
        }
    }
    cemu_hm17_increment_address(&st->hm17, 0xFFu,
                                  HM17CM4096_HEIGHT - 1u);
    write.next_x = st->hm17.x;
    write.next_y = st->hm17.y;
    write.pack_phase = st->pack_count;
    write.storage_writes = (unsigned)(st->common.data_seq - before_writes);
    write.terminal = st->hm17.window
        ? st->hm17.x == st->hm17.window_start_x &&
          st->hm17.y == st->hm17.window_start_y
        : x == end_x && y == end_y;
    cemu_lcd_sweep_finish_write(p, s, &st->common, &st->sweep, &write,
                                &HM17CM4096_OPS);
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
    hm17cm4096_state_t *st = (hm17cm4096_state_t *)p->state;
    *known = 1;
    if ((value & 0xF8u) == 0xF0u) {
        st->hm17.re = value & 7u;
        return "re_register";
    }

    unsigned channel, index;
    int upper;
    uint8_t address = value & 0x0Fu;
    if (palette_target(st->hm17.re, value >> 4, &channel, &index, &upper)) {
        index += st->palette_upper ? 8u : 0u;
        if (upper)
            st->palette[channel][index] =
                (uint8_t)((st->palette[channel][index] & 0x0Fu) |
                          ((value & 1u) << 4));
        else
            st->palette[channel][index] =
                (uint8_t)((st->palette[channel][index] & 0x10u) |
                          (value & 0x0Fu));
        return "gradation_palette";
    }

    if (st->hm17.re == 0) {
        switch (value >> 4) {
        case 0x0: st->hm17.x = (uint8_t)((st->hm17.x & 0xF0u) | address);
                  st->hm17.window_start_x = st->hm17.x;
                  st->pack_count = 0; return "x_address_lower";
        case 0x1: st->hm17.x = (uint8_t)((st->hm17.x & 0x0Fu) | (address << 4));
                  st->hm17.window_start_x = st->hm17.x;
                  st->pack_count = 0; return "x_address_upper";
        case 0x2: st->hm17.y = (uint8_t)((st->hm17.y & 0xF0u) | address);
                  st->hm17.window_start_y = st->hm17.y;
                  st->pack_count = 0; return "y_address_lower";
        case 0x3: st->hm17.y = (uint8_t)((st->hm17.y & 0x0Fu) | (address << 4));
                  st->hm17.window_start_y = st->hm17.y;
                  st->pack_count = 0; return "y_address_upper";
        case 0x4: st->hm17.start_line = (uint8_t)((st->hm17.start_line & 0xF0u) | address);
                  return "start_line_lower";
        case 0x5: st->hm17.start_line = (uint8_t)((st->hm17.start_line & 0x0Fu) | (address << 4));
                  return "start_line_upper";
        case 0x6: st->hm17.n_line = (uint8_t)((st->hm17.n_line & 0xF0u) | address);
                  return "n_line_lower";
        case 0x7: st->hm17.n_line = (uint8_t)((st->hm17.n_line & 0x0Fu) | (address << 4));
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
            if (value & 1u) cemu_hm17cm4096_reset(p, s);
            return (value & 1u) ? "all_clear" : "power_control";
        case 0xC: st->hm17.duty = address; return "lcd_duty";
        case 0xD: st->hm17.boost = value & 7u; return "boost";
        case 0xE: st->hm17.bias = value & 7u; return "bias";
        default: break;
        }
    } else if (st->hm17.re == 4) {
        switch (value >> 4) {
        case 0x6:
            st->hm17.scan_start = address;
            return "display_scan_start";
        case 0x7: return "display_signal_output";
        case 0x8:
            st->hm17.fixed_pwm = (value >> 3) & 1u;
            st->color_256 = (value >> 2) & 1u;
            return "display_selection";
        case 0x9:
            st->high_speed_write = (value >> 3) & 1u;
            st->absolute_12bit = (value >> 2) & 1u;
            st->hm17.clock_select = (value >> 1) & 1u;
            st->hm17.word_length_16 = value & 1u;
            st->pack_count = 0;
            return "ram_data_length";
        case 0xA:
            st->hm17.electric_volume =
                (uint8_t)((st->hm17.electric_volume & 0x70u) | (value & 0x0Fu));
            return "electric_volume_lower";
        case 0xB:
            st->hm17.electric_volume =
                (uint8_t)((st->hm17.electric_volume & 0x0Fu) |
                          ((value & 7u) << 4));
            return "electric_volume_upper";
        case 0xC: return "register_read_address";
        case 0xD: return "oscillator_rf";
        case 0xE: st->hm17.discharge = value & 1u; return "discharge";
        default: break;
        }
    } else if (st->hm17.re == 5) {
        switch (value >> 4) {
        case 0x0: st->hm17.end_x = (uint8_t)((st->hm17.end_x & 0xF0u) | address);
                  return "window_end_x_lower";
        case 0x1: st->hm17.end_x = (uint8_t)((st->hm17.end_x & 0x0Fu) | (address << 4));
                  return "window_end_x_upper";
        case 0x2: st->hm17.end_y = (uint8_t)((st->hm17.end_y & 0xF0u) | address);
                  return "window_end_y_lower";
        case 0x3: st->hm17.end_y = (uint8_t)((st->hm17.end_y & 0x0Fu) | (address << 4));
                  return "window_end_y_upper";
        case 0x4: st->hm17.line_start = (uint8_t)((st->hm17.line_start & 0xF0u) | address);
                  return "line_start_lower";
        case 0x5: st->hm17.line_start = (uint8_t)((st->hm17.line_start & 0x0Fu) | (address << 4));
                  return "line_start_upper";
        case 0x6: st->hm17.line_end = (uint8_t)((st->hm17.line_end & 0xF0u) | address);
                  return "line_end_lower";
        case 0x7: st->hm17.line_end = (uint8_t)((st->hm17.line_end & 0x0Fu) | (address << 4));
                  return "line_end_upper";
        case 0x8:
            st->blink_type = (value >> 1) & 1u;
            st->line_reverse = value & 1u;
            return "line_inversion_control";
        case 0x9: st->palette_upper = value & 1u;
                  return "palette_selection";
        case 0xA: st->pwm_control = address; return "pwm_control";
        default: break;
        }
    }

    *known = 0;
    return "unknown";
}

void cemu_hm17cm4096_write_byte(peripheral_t *p, soc_t *s,
                           int is_data, uint8_t value) {
    hm17cm4096_state_t *st = (hm17cm4096_state_t *)p->state;
    if (st->common.reset_asserted) return;
    if (is_data) {
        if (st->hm17.word_length_16) {
            cemu_lcd_common_emit_unsupported(&st->common, s, 16, 1,
                                         &HM17CM4096_OPS);
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
                              st->color_256 != ((value >> 2) & 1u);
    else if (st->hm17.re == 4 && group == 0x9)
        addressing_mutation = st->high_speed_write != ((value >> 3) & 1u) ||
            st->absolute_12bit != ((value >> 2) & 1u) ||
            st->hm17.word_length_16 != (value & 1u);
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
                                 &HM17CM4096_OPS);
    int known;
    const char *name = decode_command(p, s, value, &known);
    cemu_lcd_common_emit_command(&st->common, s, st, value, known, name,
                            trace_command_extra);
}

static void present(peripheral_t *p) {
    hm17cm4096_state_t *st = p->state;
    memcpy(st->presented_gram, st->gram, sizeof st->presented_gram);
}

static void close_transaction(peripheral_t *p, soc_t *s) {
    hm17cm4096_state_t *st = p->state;
    cemu_lcd_sweep_close_transaction(p, s, &st->common, &st->sweep,
                                     &HM17CM4096_OPS, "hm17cm4096");
}

static void discard_transaction(peripheral_t *p) {
    hm17cm4096_state_t *st = p->state;
    cemu_lcd_sweep_discard_transaction(&st->common, &st->sweep);
}

static int ssc_supported(peripheral_t *p, unsigned frame_bits,
                         int msb_first, int is_data) {
    hm17cm4096_state_t *st = p->state;
    return msb_first && (frame_bits == 8 || frame_bits == 16) &&
           (frame_bits != 16 || (is_data && !st->hm17.word_length_16));
}

static void ssc_consume(peripheral_t *p, soc_t *s, uint16_t tx,
                        unsigned frame_bits, int msb_first, int is_data) {
    (void)msb_first;
    if (frame_bits == 16) {
        /* M55 combines two serial octets in one 16-clock SSC data frame. */
        cemu_hm17cm4096_write_byte(p, s, 1, (uint8_t)(tx >> 8));
        cemu_hm17cm4096_write_byte(p, s, 1, (uint8_t)tx);
    } else {
        cemu_hm17cm4096_write_byte(p, s, is_data, (uint8_t)tx);
    }
}

static void hm17cm4096_tick(peripheral_t *p, soc_t *s, int n) {
    hm17cm4096_state_t *st = p->state;
    cemu_lcd_common_tick(p, s, n, &st->common, &HM17CM4096_OPS);
}

static uint64_t hm17cm4096_next_event(peripheral_t *p, soc_t *s) {
    hm17cm4096_state_t *st = p->state;
    return cemu_lcd_common_next_event(p, s, &st->common, &HM17CM4096_OPS);
}

void cemu_hm17cm4096_ssc_start(void *ctx, soc_t *s, uint16_t tx,
                          unsigned frame_bits, int msb_first) {
    peripheral_t *p = ctx;
    hm17cm4096_state_t *st = p->state;
    cemu_lcd_common_ssc_start(p, s, tx, frame_bits, msb_first,
                         &st->common, &HM17CM4096_OPS);
}

uint16_t cemu_hm17cm4096_ssc_complete(void *ctx, soc_t *s, uint16_t tx,
                                 unsigned frame_bits, int msb_first) {
    peripheral_t *p = ctx;
    hm17cm4096_state_t *st = p->state;
    return cemu_lcd_common_ssc_complete(p, s, tx, frame_bits, msb_first,
                                   &st->common, &HM17CM4096_OPS);
}

void cemu_hm17cm4096_ssc_abort(void *ctx, soc_t *s) {
    (void)s;
    peripheral_t *p = ctx;
    hm17cm4096_state_t *st = p->state;
    cemu_lcd_common_ssc_abort(&st->common);
}

int cemu_hm17cm4096_flush_pending_frame(peripheral_t *p, soc_t *s) {
    hm17cm4096_state_t *st = p->state;
    return cemu_lcd_common_flush_pending_frame(p, s, &st->common,
                                          &HM17CM4096_OPS);
}

static unsigned scan_offset(uint8_t scan_start) {
    static const uint8_t OFFSETS[16] = {
        0, 1, 9, 14, 17, 25, 33, 41, 49, 57, 65, 73, 122, 130, 138, 146
    };
    return OFFSETS[scan_start & 0x0Fu];
}

static uint8_t channel_level(const hm17cm4096_state_t *st,
                             unsigned channel, unsigned nibble) {
    if (st->hm17.monochrome) return (nibble & 8u) ? 255u : 0u;
    if (st->hm17.fixed_pwm) {
        unsigned steps = channel == 2 ? 3u : 7u;
        unsigned value = channel == 2 ? nibble >> 2 : nibble >> 1;
        return (uint8_t)((value * 255u + steps / 2u) / steps);
    }
    return (uint8_t)((st->palette[channel][nibble & 0xFu] * 255u + 15u) / 31u);
}

static void pixel_rgb(const void *state, unsigned x, unsigned y,
                      int raw, uint8_t out[3]) {
    const hm17cm4096_state_t *st = state;
    unsigned source_x = x, source_y = y;
    if (!raw) {
        if (st->common.panel_mirror_x) x = st->common.panel_width - 1u - x;
        if (st->common.panel_mirror_y) y = st->common.panel_height - 1u - y;
        source_x = st->common.panel_origin_x + x;
        unsigned line = st->common.panel_origin_y + y;
        if (st->hm17.common_shift) line = HM17CM4096_HEIGHT - 1u - line;
        source_y = (st->hm17.start_line + scan_offset(st->hm17.scan_start) + line) %
                   HM17CM4096_HEIGHT;
    }
    const uint16_t *memory = raw ? st->gram : st->presented_gram;
    uint16_t pixel = (source_x < HM17CM4096_WIDTH &&
                      source_y < HM17CM4096_HEIGHT)
                   ? memory[source_y * HM17CM4096_WIDTH + source_x] : 0;
    unsigned r = (pixel >> 8) & 0xFu;
    unsigned g = (pixel >> 4) & 0xFu;
    unsigned b = pixel & 0xFu;
    if (raw) {
        out[0] = (uint8_t)(r * 17u);
        out[1] = (uint8_t)(g * 17u);
        out[2] = (uint8_t)(b * 17u);
        return;
    }
    if (st->common.reset_asserted || !st->hm17.display_on || st->hm17.halt) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    if (st->hm17.all_on) {
        out[0] = out[1] = out[2] = 255;
        return;
    }
    out[0] = channel_level(st, 0, r);
    out[1] = channel_level(st, 1, g);
    out[2] = channel_level(st, 2, b);
    if (st->hm17.reverse) {
        out[0] = (uint8_t)(255u - out[0]);
        out[1] = (uint8_t)(255u - out[1]);
        out[2] = (uint8_t)(255u - out[2]);
    }
}

int cemu_hm17cm4096_render_rgb(const hm17cm4096_state_t *st, int raw,
                          unsigned scale, uint8_t *rgb, size_t rgb_len) {
    return st ? cemu_lcd_common_render_rgb(&st->common, st, raw,
                                       HM17CM4096_WIDTH,
                                       HM17CM4096_HEIGHT, scale,
                                       rgb, rgb_len, pixel_rgb) : -1;
}

void cemu_hm17cm4096_periph_init(peripheral_t *p, hm17cm4096_state_t *st,
                            const device_lcd_config_t *cfg) {
    memset(p, 0, sizeof *p);
    memset(st, 0, sizeof *st);
    cemu_lcd_common_init(&st->common, cfg,
                     HM17CM4096_WIDTH, HM17CM4096_HEIGHT);
    reset_registers(st);

    p->id = "lcd";
    p->model = "hm17cm4096";
    p->state = st;
    p->tick = hm17cm4096_tick;
    p->next_event_ticks = hm17cm4096_next_event;
    p->advance_quiet = cemu_lcd_common_advance_quiet;
}

static const lcd_controller_ops_t HM17CM4096_OPS = {
    .select_detail = "HM17CM4096 CS",
    .reset_detail = "HM17CM4096 RES",
    .frame_detail = "HM17CM4096 GRAM frame",
    .unsupported_detail = "HM17CM4096 unsupported serial transfer",
    .select_reports_reset = 1,
    .track_reset = 1,
    .reset = cemu_hm17cm4096_reset,
    .write_byte = cemu_hm17cm4096_write_byte,
    .ssc_supported = ssc_supported,
    .ssc_consume = ssc_consume,
    .close_transaction = close_transaction,
    .discard_transaction = discard_transaction,
    .present = present,
};
