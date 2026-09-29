/* Philips PCF8813 LCD controller: commands, DDRAM, status, and serial paths. */
#include <stdio.h>
#include <string.h>
#include "pcf8813.h"
#include "soc.h"

#define PENDING_NONE            0u
#define PENDING_DISPLAY_CONFIG  1u
#define PENDING_DATA_LENGTH     2u
#define PENDING_MAX_Y           3u
#define PENDING_MAX_X           4u
#define PENDING_START_ROW       5u
#define PENDING_PARTIAL_MODE    6u
#define PENDING_RAM_LINE        7u
#define PENDING_RESERVED        8u
#define PENDING_PARTIAL_MASK    9u

static uint64_t live_icount(const soc_t *s) {
    return s && s->cpu ? s->cpu->icount : 0;
}

static const lcd_controller_ops_t PCF8813_OPS;

static void clear_transaction(pcf8813_state_t *st) {
    cemu_lcd_sweep_reset(&st->common, &st->sweep);
    st->transaction_started = 0;
    st->transaction_sweep_member = 0;
    st->sweep_active = 0;
    st->sweep_data_bytes = 0;
}

static void discard_transaction(peripheral_t *p) {
    clear_transaction(p->state);
}

static void trace_command_extra(cemu_native_trace_event_t *ev, const void *state) {
    const pcf8813_state_t *st = state;
    cemu_event_field_bool(&ev->info, "extended", st->extended);
    cemu_event_field_i64(&ev->info, "x", st->x);
    cemu_event_field_i64(&ev->info, "y", st->y);
}

static void emit_status(pcf8813_state_t *st, soc_t *s) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "lcd_status")) return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "lcd_status";
    ev.icount = live_icount(s);
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_size = 1; ev.size = 1;
    ev.has_value = 1; ev.value = st->status_value;
    ev.detail = "PCF8813 status read";
    cemu_event_field_bool(&ev.info, "busy", 0);
    cemu_event_field_i64(&ev.info, "device", 0);
    cemu_event_field_bool(&ev.info, "display_on", (st->status_value & 0x40u) != 0);
    cemu_event_field_i64(&ev.info, "manufacturer", 0);
    cemu_event_emit_native_trace(&s->instrumentation, &ev, CEMU_EVENT_PERIPHERAL);
}

static void emit_reset(soc_t *s) {
    if (!s || !cemu_event_native_trace_active(&s->instrumentation, "lcd_reset")) return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "lcd_reset";
    ev.icount = live_icount(s);
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.detail = "PCF8813 hardware/software reset";
    cemu_event_emit_native_trace(&s->instrumentation, &ev, CEMU_EVENT_PERIPHERAL);
}

static void release_status_line(pcf8813_state_t *st, soc_t *s) {
    if (st->status_active && s)
        cemu_soc_port_input_release(s, SOC_PORT_P7, st->common.gpio_data_bit);
    st->status_active = 0;
    st->status_bit = -1;
}

static void reset_registers(pcf8813_state_t *st) {
    st->x = 0;
    st->y = 0;
    st->x_max = 101;
    st->y_max = 8;
    st->power_down = 1;
    st->vertical = 0;
    st->extended = 0;
    st->mirror_x = 0;
    st->mirror_y = 0;
    st->display_mode = PCF8813_DISPLAY_BLANK;
    st->prs = 0;
    st->power_control = 1;
    st->temperature = 0;
    st->hv_stages = 0;
    st->bias = 0;
    st->vop = 0;
    st->data_order = 0;
    st->bottom_row_swap = 0;
    st->normal_mode = 1;
    st->partial_mask = 0xFF;
    st->initial_row = 0;
    st->ram_start_line = 0;
    st->pending_command = PENDING_NONE;
    cemu_lcd_common_reset_serial(&st->common);
    st->status_requested = 0;
    st->status_active = 0;
    st->status_bit = -1;
    st->status_value = 0;
    clear_transaction(st);
}

void cemu_pcf8813_reset(peripheral_t *p, soc_t *s) {
    pcf8813_state_t *st = (pcf8813_state_t *)p->state;
    release_status_line(st, s);
    reset_registers(st);
    emit_reset(s);
}

static unsigned bank_index(unsigned y) {
    return y == 10 ? 9u : y;
}

static int valid_y(unsigned y) {
    return y <= 8u || y == 10u;
}

static uint8_t reverse_bits(uint8_t value) {
    value = (uint8_t)((value >> 4) | (value << 4));
    value = (uint8_t)(((value & 0xCCu) >> 2) | ((value & 0x33u) << 2));
    return (uint8_t)(((value & 0xAAu) >> 1) | ((value & 0x55u) << 1));
}

static void increment_address(pcf8813_state_t *st) {
    if (st->y == 10) {
        if (++st->x > st->x_max) st->x = 0;
        return;
    }
    if (st->vertical) {
        if (++st->y > st->y_max) {
            st->y = 0;
            if (++st->x > st->x_max) st->x = 0;
        }
    } else if (++st->x > st->x_max) {
        st->x = 0;
        if (++st->y > st->y_max) st->y = 0;
    }
}

static const char *decode_parameter(pcf8813_state_t *st, uint8_t value,
                                    int *known) {
    uint8_t pending = st->pending_command;
    st->pending_command = PENDING_NONE;
    switch (pending) {
    case PENDING_DISPLAY_CONFIG:
        if ((value & 0xFCu) == 0) {
            st->data_order = (value >> 1) & 1u;
            st->bottom_row_swap = value & 1u;
            return "display_configuration_parameter";
        }
        break;
    case PENDING_DATA_LENGTH:
        return "display_data_length_parameter";
    case PENDING_MAX_Y:
        if ((value & 0xF0u) == 0 && value <= 8u) {
            st->y_max = value;
            if (st->y != 10 && st->y > st->y_max) st->y = 0;
            return "maximum_y_parameter";
        }
        break;
    case PENDING_MAX_X:
        if ((value & 0x80u) == 0 && value <= 101u) {
            st->x_max = value;
            if (st->x > st->x_max) st->x = 0;
            return "maximum_x_parameter";
        }
        break;
    case PENDING_START_ROW:
        if ((value & 0x80u) == 0 && value <= 66u) {
            st->initial_row = value;
            return "initial_row_parameter";
        }
        break;
    case PENDING_PARTIAL_MODE:
        if ((value & 0xFEu) == 0) {
            st->normal_mode = value & 1u;
            return "partial_mode_parameter";
        }
        break;
    case PENDING_RAM_LINE:
        if ((value & 0x80u) == 0 && value <= 66u) {
            st->ram_start_line = value;
            return "ram_line_parameter";
        }
        break;
    case PENDING_PARTIAL_MASK:
        st->partial_mask = value;
        return "partial_mask_parameter";
    case PENDING_RESERVED:
        break;
    default:
        break;
    }
    *known = 0;
    return pending == PENDING_RESERVED ? "reserved_parameter" : "invalid_parameter";
}

static const char *decode_command(peripheral_t *p, soc_t *s, uint8_t value,
                                  int *known) {
    pcf8813_state_t *st = (pcf8813_state_t *)p->state;
    *known = 1;
    if (st->pending_command != PENDING_NONE)
        return decode_parameter(st, value, known);

    if (value == 0x00) return "nop";
    if ((value & 0xFCu) == 0x18u) {
        st->status_requested = 1;
        return "read_status";
    }
    if ((value & 0xE0u) == 0x20u) {
        st->mirror_x = (value >> 4) & 1u;
        st->mirror_y = (value >> 3) & 1u;
        st->power_down = (value >> 2) & 1u;
        st->vertical = (value >> 1) & 1u;
        st->extended = value & 1u;
        return "function_set";
    }

    if (!st->extended) {
        if (value == 0x08 || value == 0x09 || value == 0x0C || value == 0x0D) {
            st->display_mode = (uint8_t)((((value >> 2) & 1u) << 1) | (value & 1u));
            return "display_control";
        }
        if (value == 0x10 || value == 0x11) {
            st->prs = value & 1u;
            return "vop_range";
        }
        if (value == 0x12 || value == 0x13) {
            st->power_control = value & 1u;
            return "power_control";
        }
        if ((value & 0xFEu) == 0x16u) {
            st->pending_command = PENDING_DISPLAY_CONFIG;
            return "display_configuration";
        }
        if ((value & 0xF0u) == 0x40u) {
            unsigned y = value & 0x0Fu;
            if (valid_y(y)) {
                st->y = (uint8_t)y;
                return "set_y";
            }
        }
        if ((value & 0xF8u) == 0x50u) {
            st->pending_command = PENDING_MAX_Y;
            return "set_maximum_y";
        }
        if ((value & 0xF8u) == 0x60u) {
            st->pending_command = PENDING_MAX_X;
            return "set_maximum_x";
        }
        if ((value & 0xF0u) == 0x70u) {
            st->pending_command = PENDING_DATA_LENGTH;
            return "set_display_data_length";
        }
        if (0x80u <= value && value <= 0xE5u) {
            st->x = value & 0x7Fu;
            return "set_x";
        }
    } else {
        if ((value & 0xFCu) == 0x04u) {
            st->temperature = value & 0x03u;
            return "temperature";
        }
        if ((value & 0xFCu) == 0x08u) {
            st->hv_stages = value & 0x03u;
            return "hv_stages";
        }
        if ((value & 0xF8u) == 0x10u) {
            st->bias = value & 0x07u;
            return "bias";
        }
        if (value == 0x1C) return "disable_otp";
        if (value == 0x1E) return "module_calibration";
        if ((value & 0xF8u) == 0x48u) {
            st->pending_command = PENDING_START_ROW;
            return "set_initial_row";
        }
        if ((value & 0xF8u) == 0x50u) {
            st->pending_command = PENDING_PARTIAL_MODE;
            return "set_partial_mode";
        }
        if ((value & 0xF8u) == 0x58u) {
            st->pending_command = PENDING_RAM_LINE;
            return "set_ram_line";
        }
        if ((value & 0xF8u) == 0x60u) {
            st->pending_command = PENDING_RESERVED;
            *known = 0;
            return "reserved_double_command";
        }
        if ((value & 0xF8u) == 0x68u) {
            st->pending_command = PENDING_PARTIAL_MASK;
            return "set_partial_mask";
        }
        if (value == 0x71) {
            cemu_pcf8813_reset(p, s);
            return "software_reset";
        }
        if (value & 0x80u) {
            st->vop = value & 0x7Fu;
            return "vop";
        }
    }

    *known = 0;
    return "unknown";
}

void cemu_pcf8813_write_byte(peripheral_t *p, soc_t *s, int is_data, uint8_t value) {
    pcf8813_state_t *st = (pcf8813_state_t *)p->state;
    if (!is_data) {
        int conflict = 0;
        if (st->pending_command == PENDING_DISPLAY_CONFIG)
            conflict = ((value >> 1) & 1u) != st->data_order;
        else if (st->pending_command == PENDING_MAX_X)
            conflict = value != st->x_max;
        else if (st->pending_command == PENDING_MAX_Y)
            conflict = value != st->y_max;
        else if ((value & 0xE0u) == 0x20u)
            conflict = ((value >> 1) & 1u) != st->vertical;
        if (conflict)
            cemu_lcd_sweep_fail_open(p, s, &st->common, &st->sweep,
                                     &PCF8813_OPS);
        int known;
        const char *name = decode_command(p, s, value, &known);
        cemu_lcd_common_emit_command(&st->common, s, st, value, known, name,
                                trace_command_extra);
        return;
    }

    unsigned logical_x = st->x, y = st->y;
    pcf8813_state_t next = *st;
    increment_address(&next);
    unsigned viewport_last_bank =
        (st->common.panel_origin_y + st->common.panel_height - 1u) / 8u;
    int terminal = !st->vertical
        ? logical_x == st->x_max && y == viewport_last_bank
        : y == viewport_last_bank && logical_x == st->x_max;
    lcd_sweep_write_t write = {
        .x = logical_x, .y = y, .next_x = next.x, .next_y = next.y,
        .start_x = 0, .start_y = 0, .end_x = st->x_max,
        .end_y = viewport_last_bank,
        .mode = (uint32_t)st->vertical | ((uint32_t)st->data_order << 1) |
                ((uint32_t)st->x_max << 8) | ((uint32_t)st->y_max << 16),
        .storage_writes = valid_y(y) && logical_x < PCF8813_WIDTH,
        .data_bytes = 1, .windowed = 0, .terminal = terminal,
    };
    cemu_lcd_sweep_prepare_write(p, s, &st->common, &st->sweep, &write,
                                 &PCF8813_OPS);
    if (logical_x <= 101u && valid_y(y)) {
        uint8_t stored = st->data_order ? reverse_bits(value) : value;
        if (y == 8) stored &= 0x07u;
        if (y == 10) stored &= 0x01u;
        st->ddram[bank_index(y) * PCF8813_WIDTH + logical_x] = stored;
        cemu_lcd_common_note_pixel(&st->common, s);
        cemu_lcd_common_emit_data(&st->common, s, st, stored, 1, logical_x, y,
                             "PCF8813 DDRAM", NULL);
    }
    increment_address(st);
    write.next_x = st->x;
    write.next_y = st->y;
    cemu_lcd_sweep_finish_write(p, s, &st->common, &st->sweep, &write,
                                &PCF8813_OPS);
}

static void present(peripheral_t *p) {
    pcf8813_state_t *st = p->state;
    memcpy(st->presented_ddram, st->ddram, sizeof st->presented_ddram);
}

static void close_transaction(peripheral_t *p, soc_t *s) {
    pcf8813_state_t *st = p->state;
    cemu_lcd_sweep_close_transaction(p, s, &st->common, &st->sweep,
                                     &PCF8813_OPS, "pcf8813");
    st->transaction_count = st->sweep.transaction_sequence;
}

static uint8_t status_byte(const pcf8813_state_t *st) {
    return (!st->power_down && st->power_control) ? 0x40u : 0x00u;
}

static void drive_status_bit(pcf8813_state_t *st, soc_t *s) {
    if (!st->status_active || st->status_bit < 0) return;
    int level = (st->status_value >> st->status_bit) & 1u;
    cemu_soc_port_input_level(s, SOC_PORT_P7, st->common.gpio_data_bit, level);
}

static void select_changed(peripheral_t *p, soc_t *s, int selected) {
    pcf8813_state_t *st = p->state;
    if (selected) {
        st->transaction_started = 0;
        st->transaction_sweep_member = 0;
        return;
    }
    release_status_line(st, s);
    st->status_requested = 0;
}

static void gpio_direction_changed(peripheral_t *p, soc_t *s, int output) {
    pcf8813_state_t *st = p->state;
    if (!output && st->status_requested) {
        st->status_requested = 0;
        st->status_active = 1;
        st->status_bit = 7;
        st->status_value = status_byte(st);
        drive_status_bit(st, s);
        emit_status(st, s);
    } else if (output) {
        release_status_line(st, s);
    }
}

static void gpio_input_falling_edge(peripheral_t *p, soc_t *s) {
    pcf8813_state_t *st = p->state;
    if (!st->status_active) return;
    if (--st->status_bit >= 0) drive_status_bit(st, s);
    else release_status_line(st, s);
}

static int ssc_supported(peripheral_t *p, unsigned frame_bits,
                         int msb_first, int is_data) {
    (void)p;
    (void)msb_first;
    (void)is_data;
    return frame_bits <= 16;
}

static void ssc_consume(peripheral_t *p, soc_t *s, uint16_t tx,
                        unsigned frame_bits, int msb_first, int is_data) {
    pcf8813_state_t *st = p->state;
    for (unsigned i = 0; i < frame_bits; i++) {
        unsigned bit_index = msb_first ? frame_bits - 1u - i : i;
        st->common.ssc_shift = (uint8_t)((st->common.ssc_shift << 1) |
                                         ((tx >> bit_index) & 1u));
        if (++st->common.ssc_shift_bits == 8) {
            cemu_pcf8813_write_byte(p, s, is_data, st->common.ssc_shift);
            st->common.ssc_shift = 0;
            st->common.ssc_shift_bits = 0;
        }
    }
}

static void pcf8813_tick(peripheral_t *p, soc_t *s, int n) {
    pcf8813_state_t *st = p->state;
    cemu_lcd_common_tick(p, s, n, &st->common, &PCF8813_OPS);
}

static uint64_t pcf8813_next_event(peripheral_t *p, soc_t *s) {
    pcf8813_state_t *st = p->state;
    return cemu_lcd_common_next_event(p, s, &st->common, &PCF8813_OPS);
}

void cemu_pcf8813_ssc_start(void *ctx, soc_t *s, uint16_t tx,
                       unsigned frame_bits, int msb_first) {
    peripheral_t *p = ctx;
    pcf8813_state_t *st = p->state;
    cemu_lcd_common_ssc_start(p, s, tx, frame_bits, msb_first,
                         &st->common, &PCF8813_OPS);
}

uint16_t cemu_pcf8813_ssc_complete(void *ctx, soc_t *s, uint16_t tx,
                              unsigned frame_bits, int msb_first) {
    peripheral_t *p = ctx;
    pcf8813_state_t *st = p->state;
    return cemu_lcd_common_ssc_complete(p, s, tx, frame_bits, msb_first,
                                   &st->common, &PCF8813_OPS);
}

void cemu_pcf8813_ssc_abort(void *ctx, soc_t *s) {
    (void)s;
    peripheral_t *p = ctx;
    pcf8813_state_t *st = p->state;
    cemu_lcd_common_ssc_abort(&st->common);
}

int cemu_pcf8813_flush_pending_frame(peripheral_t *p, soc_t *s) {
    pcf8813_state_t *st = p->state;
    return cemu_lcd_common_flush_pending_frame(p, s, &st->common,
                                          &PCF8813_OPS);
}

static int ram_pixel(const pcf8813_state_t *st, unsigned x, unsigned row,
                     int raw) {
    const uint8_t *memory = raw ? st->ddram : st->presented_ddram;
    if (row == 67)
        return memory[9u * PCF8813_WIDTH + x] & 1u;
    return (memory[(row >> 3) * PCF8813_WIDTH + x] >> (row & 7u)) & 1u;
}

static int visible_pixel(const pcf8813_state_t *st, unsigned x, unsigned y,
                         int raw_ddram) {
    unsigned source_x = x;
    unsigned source_row = y;
    if (!raw_ddram) {
        x += st->common.panel_origin_x;
        y += st->common.panel_origin_y;
        unsigned controller_x = st->common.panel_mirror_x ? 101u - x : x;
        unsigned controller_row = st->common.panel_mirror_y ? 67u - y : y;
        source_x = st->mirror_x ? 101u - controller_x : controller_x;
        unsigned logical_row = st->mirror_y ? 67u - controller_row : controller_row;
        if (logical_row == 67) source_row = 67;
        else {
            if (!st->normal_mode && !(st->partial_mask & (1u << (logical_row >> 3))))
                return 0;
            int mapped = (int)logical_row - (int)st->initial_row + st->ram_start_line;
            while (mapped < 0) mapped += 67;
            source_row = (unsigned)mapped % 67u;
        }
    }
    return ram_pixel(st, source_x, source_row, raw_ddram);
}

static void pixel_rgb(const void *state, unsigned x, unsigned y,
                      int raw, uint8_t out[3]) {
    static const uint8_t OFF[3] = { 202, 214, 184 };
    static const uint8_t ON[3] = { 30, 39, 32 };
    const pcf8813_state_t *st = state;
    int pixel = visible_pixel(st, x, y, raw);
    if (!raw) {
        if (st->power_down || !st->power_control ||
            st->display_mode == PCF8813_DISPLAY_BLANK) pixel = 0;
        else if (st->display_mode == PCF8813_DISPLAY_ALL_ON) pixel = 1;
        else if (st->display_mode == PCF8813_DISPLAY_INVERSE) pixel = !pixel;
    }
    const uint8_t *color = pixel ? ON : OFF;
    out[0] = color[0];
    out[1] = color[1];
    out[2] = color[2];
}

int cemu_pcf8813_render_rgb(const pcf8813_state_t *st, unsigned scale,
                       uint8_t *rgb, size_t rgb_len) {
    return st ? cemu_lcd_common_render_rgb(&st->common, st, 0,
                                      PCF8813_WIDTH, PCF8813_HEIGHT, scale,
                                      rgb, rgb_len, pixel_rgb) : -1;
}

int cemu_pcf8813_render_ddram_rgb(const pcf8813_state_t *st, unsigned scale,
                             uint8_t *rgb, size_t rgb_len) {
    return st ? cemu_lcd_common_render_rgb(&st->common, st, 1,
                                      PCF8813_WIDTH, PCF8813_HEIGHT, scale,
                                      rgb, rgb_len, pixel_rgb) : -1;
}

void cemu_pcf8813_periph_init(peripheral_t *p, pcf8813_state_t *st,
                         const device_lcd_config_t *cfg) {
    memset(p, 0, sizeof(*p));
    memset(st, 0, sizeof(*st));
    cemu_lcd_common_init(&st->common, cfg, PCF8813_WIDTH, PCF8813_HEIGHT);
    reset_registers(st);

    p->id = "lcd";
    p->model = "pcf8813";
    p->state = st;
    p->sfr_words = NULL; p->n_sfr_words = 0;
    p->byte_ranges = NULL; p->n_byte_ranges = 0;
    p->ic_nodes = NULL; p->n_ic_nodes = 0;
    p->reg_names = NULL; p->n_reg_names = 0;
    p->read8 = NULL; p->peek8 = NULL; p->write8 = NULL;
    p->read_sfr_word = NULL; p->on_sfr_poll = NULL; p->on_sfr_write = NULL;
    p->tick = pcf8813_tick;
    p->next_event_ticks = pcf8813_next_event;
    p->advance_quiet = cemu_lcd_common_advance_quiet;
    p->timer_running = NULL;
    p->ic_will_fire = NULL;
}

static const lcd_controller_ops_t PCF8813_OPS = {
    .select_detail = "PCF8813 SCE",
    .reset_detail = "PCF8813 RES",
    .frame_detail = "PCF8813 DDRAM frame",
    .unsupported_detail = "PCF8813 unsupported serial transfer",
    .data_active_high = 1,
    .reset = cemu_pcf8813_reset,
    .write_byte = cemu_pcf8813_write_byte,
    .ssc_supported = ssc_supported,
    .ssc_consume = ssc_consume,
    .select_changed = select_changed,
    .gpio_direction_changed = gpio_direction_changed,
    .gpio_input_falling_edge = gpio_input_falling_edge,
    .close_transaction = close_transaction,
    .discard_transaction = discard_transaction,
    .present = present,
};
