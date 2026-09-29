/* Controller-neutral LCD wiring, transport, tracing, and rendering. */
#include <string.h>

#include "lcd_common.h"
#include "soc.h"

static uint64_t live_icount(const soc_t *s) {
    return s && s->cpu ? s->cpu->icount : 0;
}

static int pin_level(const soc_t *s, uint32_t addr, int bit) {
    return (memory_controller_sfr_get(&s->memory, addr) >> bit) & 1;
}

static int is_data_level(const lcd_common_state_t *st, const soc_t *s,
                         const lcd_controller_ops_t *ops) {
    int dc = pin_level(s, st->dc_addr, st->dc_bit);
    return ops->data_active_high ? dc : !dc;
}

static void emit_select(lcd_common_state_t *st, soc_t *s, int level,
                        const lcd_controller_ops_t *ops) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "lcd_select"))
        return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "lcd_select";
    ev.icount = live_icount(s);
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = st->select_addr;
    ev.has_size = 1; ev.size = 1;
    ev.has_value = 1; ev.value = (uint32_t)level;
    ev.detail = ops->select_detail;
    cemu_event_field_i64(&ev.info, "level", level);
    cemu_event_field_bool(&ev.info, "selected", st->selected);
    if (ops->select_reports_reset)
        cemu_event_field_bool(&ev.info, "reset", st->reset_asserted);
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                               CEMU_EVENT_PERIPHERAL);
}

void cemu_lcd_common_emit_reset(lcd_common_state_t *st, soc_t *s, int asserted,
                           const lcd_controller_ops_t *ops) {
    if (!s ||
        !cemu_event_native_trace_active(&s->instrumentation, "lcd_reset"))
        return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "lcd_reset";
    ev.icount = live_icount(s);
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_value = 1; ev.value = (uint32_t)asserted;
    ev.detail = ops->reset_detail;
    cemu_event_field_bool(&ev.info, "asserted", asserted);
    cemu_event_field_i64(&ev.info, "capcom_channel", st->reset_capcom_channel);
    cemu_event_field_bool(&ev.info, "capcom_driven",
                    st->reset_capcom_channel >= 0 &&
                    cemu_soc_capcom_output_driven(s, st->reset_capcom_channel));
    cemu_event_field_i64(&ev.info, "idle_level", st->reset_capcom_idle_level);
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                               CEMU_EVENT_PERIPHERAL);
}

void cemu_lcd_common_emit_command(lcd_common_state_t *st, soc_t *s,
                             const void *state, uint8_t value, int known,
                             const char *name, lcd_trace_extra_fn extra) {
    (void)st;
    if (!cemu_event_native_trace_active(&s->instrumentation, "lcd_command"))
        return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "lcd_command";
    ev.icount = live_icount(s);
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_size = 1; ev.size = 1;
    ev.has_value = 1; ev.value = value;
    ev.detail = name;
    cemu_event_field_bool(&ev.info, "known", known);
    if (extra) extra(&ev, state);
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                               CEMU_EVENT_PERIPHERAL);
}

void cemu_lcd_common_emit_data(lcd_common_state_t *st, soc_t *s,
                          const void *state, uint32_t value, unsigned size,
                          unsigned x, unsigned y, const char *detail,
                          lcd_trace_extra_fn extra) {
    (void)st;
    if (!cemu_event_native_trace_active(&s->instrumentation, "lcd_data"))
        return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "lcd_data";
    ev.icount = live_icount(s);
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_size = 1; ev.size = size;
    ev.has_value = 1; ev.value = value;
    ev.detail = detail;
    cemu_event_field_i64(&ev.info, "x", (long)x);
    cemu_event_field_i64(&ev.info, "y", (long)y);
    if (extra) extra(&ev, state);
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                               CEMU_EVENT_PERIPHERAL);
}

void cemu_lcd_common_emit_unsupported(lcd_common_state_t *st, soc_t *s,
                                 unsigned frame_bits, int msb_first,
                                 const lcd_controller_ops_t *ops) {
    st->unsupported_transfers++;
    if (!cemu_event_native_trace_active(&s->instrumentation, "lcd_command"))
        return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "lcd_command";
    ev.icount = live_icount(s);
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_value = 1; ev.value = frame_bits;
    ev.detail = ops->unsupported_detail;
    cemu_event_field_i64(&ev.info, "frame_bits", frame_bits);
    cemu_event_field_bool(&ev.info, "msb_first", msb_first);
    cemu_event_field_i64(&ev.info, "count", st->unsupported_transfers);
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                               CEMU_EVENT_PERIPHERAL);
}

static void emit_frame(lcd_common_state_t *st, soc_t *s, int flushed,
                       uint32_t data_bytes, const char *boundary,
                       unsigned start_x, unsigned start_y,
                       unsigned end_x, unsigned end_y,
                       uint32_t storage_writes,
                       const lcd_controller_ops_t *ops) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "lcd_frame"))
        return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "lcd_frame";
    ev.icount = live_icount(s);
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_value = 1; ev.value = (uint32_t)st->frame_seq;
    ev.detail = ops->frame_detail;
    cemu_event_field_i64(&ev.info, "data_bytes", data_bytes);
    cemu_event_field_string(&ev.info, "boundary_reason", boundary);
    cemu_event_field_bool(&ev.info, "flushed", flushed);
    cemu_event_field_i64(&ev.info, "sequence", (long)st->frame_seq);
    cemu_event_field_i64(&ev.info, "start_x", (long)start_x);
    cemu_event_field_i64(&ev.info, "start_y", (long)start_y);
    cemu_event_field_i64(&ev.info, "end_x", (long)end_x);
    cemu_event_field_i64(&ev.info, "end_y", (long)end_y);
    cemu_event_field_i64(&ev.info, "storage_writes", storage_writes);
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                               CEMU_EVENT_PERIPHERAL);
}

static int reset_is_asserted(const lcd_common_state_t *st, soc_t *s) {
    if (st->reset_capcom_channel < 0) return 0;
    int level = st->reset_capcom_idle_level != 0;
    if (cemu_soc_capcom_output_driven(s, st->reset_capcom_channel))
        level = cemu_soc_capcom_output_level(s, st->reset_capcom_channel);
    return !level;
}

void cemu_lcd_common_publish_frame(lcd_common_state_t *st, soc_t *s,
                               uint32_t data_bytes,
                               const lcd_controller_ops_t *ops) {
    if (!data_bytes) return;
    st->frame_seq++;
    st->last_frame_icount = live_icount(s);
    emit_frame(st, s, 0, data_bytes, "fallback", 0, 0, 0, 0, 0, ops);
}

void cemu_lcd_common_emit_transaction(lcd_common_state_t *st, soc_t *s,
                                  uint64_t sequence, uint32_t data_bytes,
                                  unsigned x, unsigned y,
                                  const char *disposition,
                                  const char *detail) {
    (void)st;
    if (!cemu_event_native_trace_active(&s->instrumentation,
                                        "lcd_transaction"))
        return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "lcd_transaction";
    ev.icount = live_icount(s);
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_value = 1; ev.value = (uint32_t)sequence;
    ev.detail = detail;
    cemu_event_field_i64(&ev.info, "data_bytes", data_bytes);
    cemu_event_field_string(&ev.info, "disposition", disposition);
    cemu_event_field_i64(&ev.info, "sequence", (long)sequence);
    cemu_event_field_i64(&ev.info, "x", (long)x);
    cemu_event_field_i64(&ev.info, "y", (long)y);
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                                 CEMU_EVENT_PERIPHERAL);
}

static void close_data_frame(peripheral_t *p, lcd_common_state_t *st,
                             soc_t *s, int flushed,
                             const lcd_controller_ops_t *ops) {
    if (!st->transaction_data_bytes) return;
    if (ops->close_transaction) {
        ops->close_transaction(p, s);
        return;
    }
    uint32_t data_bytes = st->transaction_data_bytes;
    st->transaction_data_bytes = 0;
    st->frame_seq++;
    st->last_frame_icount = live_icount(s);
    emit_frame(st, s, flushed, data_bytes, "fallback", 0, 0, 0, 0, 0,
               ops);
}

static int sweep_shape_matches(const lcd_sweep_tracker_t *tracker,
                               const lcd_sweep_write_t *write) {
    return tracker->mode == write->mode &&
           tracker->windowed == !!write->windowed &&
           tracker->start_x == write->start_x &&
           tracker->start_y == write->start_y &&
           tracker->end_x == write->end_x &&
           tracker->end_y == write->end_y &&
           tracker->pack_phase == write->pack_phase;
}

void cemu_lcd_sweep_publish(peripheral_t *p, soc_t *s,
                          lcd_common_state_t *st,
                          lcd_sweep_tracker_t *tracker,
                          const lcd_controller_ops_t *ops,
                          const char *boundary) {
    if (!tracker->active) return;
    if (!tracker->storage_writes) {
        tracker->active = 0;
        tracker->data_bytes = 0;
        return;
    }
    if (ops->present) ops->present(p);
    st->frame_seq++;
    st->last_frame_icount = live_icount(s);
    emit_frame(st, s, 0, tracker->data_bytes, boundary,
               tracker->start_x, tracker->start_y,
               tracker->end_x, tracker->end_y,
               tracker->storage_writes, ops);
    tracker->active = 0;
    tracker->data_bytes = 0;
    tracker->storage_writes = 0;
}

void cemu_lcd_sweep_reset(lcd_common_state_t *st,
                          lcd_sweep_tracker_t *tracker) {
    if (st) st->transaction_data_bytes = 0;
    if (tracker) {
        uint64_t sequence = tracker->transaction_sequence;
        memset(tracker, 0, sizeof *tracker);
        tracker->transaction_sequence = sequence;
    }
}

void cemu_lcd_sweep_prepare_write(peripheral_t *p, soc_t *s,
                                  lcd_common_state_t *st,
                                  lcd_sweep_tracker_t *tracker,
                                  const lcd_sweep_write_t *write,
                                  const lcd_controller_ops_t *ops) {
    if (!tracker->transaction_started) {
        tracker->transaction_started = 1;
        tracker->transaction_x = (uint16_t)write->x;
        tracker->transaction_y = (uint16_t)write->y;
        tracker->transaction_frame_sequence = st->frame_seq;
    }
    if (tracker->active &&
        (tracker->expected_x != write->x ||
         tracker->expected_y != write->y ||
         !sweep_shape_matches(tracker, write))) {
        const char *reason =
            sweep_shape_matches(tracker, write) &&
            tracker->origin_x == write->x && tracker->origin_y == write->y
            ? "origin_restart" : "fallback";
        cemu_lcd_sweep_publish(p, s, st, tracker, ops, reason);
    }
    if (!tracker->active) {
        tracker->active = 1;
        tracker->origin_x = (uint16_t)write->x;
        tracker->origin_y = (uint16_t)write->y;
        tracker->start_x = (uint16_t)write->start_x;
        tracker->start_y = (uint16_t)write->start_y;
        tracker->end_x = (uint16_t)write->end_x;
        tracker->end_y = (uint16_t)write->end_y;
        tracker->mode = write->mode;
        tracker->windowed = !!write->windowed;
        tracker->pack_phase = (uint8_t)write->pack_phase;
    }
}

void cemu_lcd_sweep_finish_write(peripheral_t *p, soc_t *s,
                                 lcd_common_state_t *st,
                                 lcd_sweep_tracker_t *tracker,
                                 const lcd_sweep_write_t *write,
                                 const lcd_controller_ops_t *ops) {
    tracker->data_bytes += write->data_bytes;
    tracker->storage_writes += write->storage_writes;
    tracker->expected_x = (uint16_t)write->next_x;
    tracker->expected_y = (uint16_t)write->next_y;
    tracker->pack_phase = (uint8_t)write->pack_phase;
    st->transaction_data_bytes += write->data_bytes;
    if (write->terminal && write->pack_phase == 0)
        cemu_lcd_sweep_publish(p, s, st, tracker, ops,
                               write->windowed ? "window_wrap"
                                               : "viewport_complete");
}

void cemu_lcd_sweep_fail_open(peripheral_t *p, soc_t *s,
                              lcd_common_state_t *st,
                              lcd_sweep_tracker_t *tracker,
                              const lcd_controller_ops_t *ops) {
    cemu_lcd_sweep_publish(p, s, st, tracker, ops, "fallback");
}

void cemu_lcd_sweep_close_transaction(peripheral_t *p, soc_t *s,
                                      lcd_common_state_t *st,
                                      lcd_sweep_tracker_t *tracker,
                                      const lcd_controller_ops_t *ops,
                                      const char *detail) {
    (void)p;
    (void)ops;
    uint32_t bytes = st->transaction_data_bytes;
    if (!bytes) return;
    tracker->transaction_sequence++;
    const char *disposition = st->frame_seq !=
                              tracker->transaction_frame_sequence
                            ? "committed" : "held";
    cemu_lcd_common_emit_transaction(st, s, tracker->transaction_sequence,
                                     bytes, tracker->transaction_x,
                                     tracker->transaction_y,
                                     disposition, detail);
    st->transaction_data_bytes = 0;
    tracker->transaction_started = 0;
}

void cemu_lcd_sweep_discard_transaction(lcd_common_state_t *st,
                                        lcd_sweep_tracker_t *tracker) {
    cemu_lcd_sweep_reset(st, tracker);
}

static void clear_partial(peripheral_t *p,
                          const lcd_controller_ops_t *ops) {
    if (ops->clear_partial) ops->clear_partial(p);
}

static void sync_reset(peripheral_t *p, soc_t *s, lcd_common_state_t *st,
                       const lcd_controller_ops_t *ops) {
    if (!ops->track_reset) return;
    int asserted = reset_is_asserted(st, s);
    if (!st->reset_known) {
        st->reset_known = 1;
        st->reset_asserted = asserted;
        if (asserted && ops->reset) ops->reset(p, s);
        else cemu_lcd_common_emit_reset(st, s, 0, ops);
        return;
    }
    if (asserted == st->reset_asserted) return;
    st->reset_asserted = asserted;
    cemu_lcd_common_reset_serial(st);
    clear_partial(p, ops);
    if (asserted) {
        if (ops->discard_transaction) ops->discard_transaction(p);
        else close_data_frame(p, st, s, 0, ops);
        if (ops->reset) ops->reset(p, s);
    } else {
        cemu_lcd_common_emit_reset(st, s, 0, ops);
    }
}

static void sync_select(peripheral_t *p, soc_t *s, lcd_common_state_t *st,
                        const lcd_controller_ops_t *ops) {
    int level = pin_level(s, st->select_addr, st->select_bit);
    int selected = st->select_active_low ? !level : level;
    if (!st->select_known) {
        st->select_known = 1;
        st->selected = selected;
        cemu_lcd_common_reset_serial(st);
        st->transaction_data_bytes = 0;
        return;
    }
    if (selected == st->selected) return;
    if (!selected) close_data_frame(p, st, s, 0, ops);
    st->selected = selected;
    cemu_lcd_common_reset_serial(st);
    clear_partial(p, ops);
    st->gpio_clock_known = 0;
    st->gpio_data_direction_known = 0;
    if (selected) st->transaction_data_bytes = 0;
    if (ops->select_changed) ops->select_changed(p, s, selected);
    emit_select(st, s, level, ops);
}

static int data_is_output(lcd_common_state_t *st, soc_t *s) {
    return !st->gpio_data_direction_addr ||
           pin_level(s, st->gpio_data_direction_addr, st->gpio_data_bit);
}

static void sync_gpio_serial(peripheral_t *p, soc_t *s,
                             lcd_common_state_t *st,
                             const lcd_controller_ops_t *ops) {
    if (!st->selected || st->reset_asserted || !st->gpio_clock_addr) return;
    int clock = pin_level(s, st->gpio_clock_addr, st->gpio_clock_bit);
    int output = data_is_output(st, s);
    if (!st->gpio_clock_known) {
        st->gpio_clock_known = 1;
        st->gpio_clock_level = clock;
    }
    if (!st->gpio_data_direction_known) {
        st->gpio_data_direction_known = 1;
        st->gpio_data_is_output = output;
    } else if (output != st->gpio_data_is_output) {
        st->gpio_data_is_output = output;
        if (ops->gpio_direction_changed)
            ops->gpio_direction_changed(p, s, output);
    }
    if (clock == st->gpio_clock_level) return;
    int rising = clock && !st->gpio_clock_level;
    st->gpio_clock_level = clock;
    if (rising && output) {
        int data = pin_level(s, st->gpio_data_addr, st->gpio_data_bit);
        st->gpio_shift = (uint8_t)((st->gpio_shift << 1) | data);
        if (++st->gpio_shift_bits == 8) {
            ops->write_byte(p, s, is_data_level(st, s, ops),
                            st->gpio_shift);
            st->gpio_shift = 0;
            st->gpio_shift_bits = 0;
        }
    } else if (!rising && !output && ops->gpio_input_falling_edge) {
        ops->gpio_input_falling_edge(p, s);
    }
}

void cemu_lcd_common_init(lcd_common_state_t *st, const device_lcd_config_t *cfg,
                     unsigned default_width, unsigned default_height) {
    memset(st, 0, sizeof *st);
    st->select_addr = cfg->select_addr;
    st->select_bit = cfg->select_bit;
    st->select_active_low = cfg->select_active_low;
    st->dc_addr = cfg->dc_addr;
    st->dc_bit = cfg->dc_bit;
    st->gpio_clock_addr = cfg->gpio_clock_addr;
    st->gpio_clock_bit = cfg->gpio_clock_bit;
    st->gpio_data_addr = cfg->gpio_data_addr;
    st->gpio_data_direction_addr = cfg->gpio_data_direction_addr;
    st->gpio_data_bit = cfg->gpio_data_bit;
    st->reset_capcom_channel = cfg->reset_capcom_channel;
    st->reset_capcom_idle_level = cfg->reset_capcom_idle_level;
    st->panel_width = cfg->panel_width ? cfg->panel_width : default_width;
    st->panel_height = cfg->panel_height ? cfg->panel_height : default_height;
    st->panel_origin_x = cfg->panel_origin_x;
    st->panel_origin_y = cfg->panel_origin_y;
    st->panel_mirror_x = cfg->panel_mirror_x;
    st->panel_mirror_y = cfg->panel_mirror_y;
}

void cemu_lcd_common_reset_serial(lcd_common_state_t *st) {
    st->gpio_shift = 0;
    st->gpio_shift_bits = 0;
    st->ssc_shift = 0;
    st->ssc_shift_bits = 0;
    st->frame_pending = 0;
}

void cemu_lcd_common_tick(peripheral_t *p, soc_t *s, int n,
                     lcd_common_state_t *st,
                     const lcd_controller_ops_t *ops) {
    (void)n;
    sync_reset(p, s, st, ops);
    sync_select(p, s, st, ops);
    sync_gpio_serial(p, s, st, ops);
}

uint64_t cemu_lcd_common_next_event(peripheral_t *p, soc_t *s,
                               lcd_common_state_t *st,
                               const lcd_controller_ops_t *ops) {
    (void)p;
    if (ops->track_reset) {
        int reset = reset_is_asserted(st, s);
        if (!st->reset_known || reset != st->reset_asserted) return 1;
    }
    int level = pin_level(s, st->select_addr, st->select_bit);
    int selected = st->select_active_low ? !level : level;
    if (!st->select_known || selected != st->selected) return 1;
    if (!selected || st->reset_asserted || !st->gpio_clock_addr)
        return UINT64_MAX;
    int clock = pin_level(s, st->gpio_clock_addr, st->gpio_clock_bit);
    int output = data_is_output(st, s);
    if (!st->gpio_clock_known || clock != st->gpio_clock_level ||
        !st->gpio_data_direction_known || output != st->gpio_data_is_output)
        return 1;
    return UINT64_MAX;
}

void cemu_lcd_common_advance_quiet(peripheral_t *p, soc_t *s, uint64_t ticks) {
    (void)p;
    (void)s;
    (void)ticks;
}

static int ssc_supported(peripheral_t *p, unsigned frame_bits,
                         int msb_first, int is_data,
                         const lcd_controller_ops_t *ops) {
    return !ops->ssc_supported ||
           ops->ssc_supported(p, frame_bits, msb_first, is_data);
}

void cemu_lcd_common_ssc_start(peripheral_t *p, soc_t *s, uint16_t tx,
                          unsigned frame_bits, int msb_first,
                          lcd_common_state_t *st,
                          const lcd_controller_ops_t *ops) {
    (void)tx;
    sync_reset(p, s, st, ops);
    sync_select(p, s, st, ops);
    st->frame_pending = 1;
    st->frame_selected = st->selected && !st->reset_asserted;
    st->frame_is_data = is_data_level(st, s, ops);
    if (!ssc_supported(p, frame_bits, msb_first, st->frame_is_data, ops)) {
        cemu_lcd_common_emit_unsupported(st, s, frame_bits, msb_first, ops);
        st->frame_selected = 0;
    }
}

static uint16_t frame_mask(unsigned frame_bits) {
    if (frame_bits >= 16) return 0xFFFFu;
    if (!frame_bits) return 0;
    return (uint16_t)((1u << frame_bits) - 1u);
}

uint16_t cemu_lcd_common_ssc_complete(peripheral_t *p, soc_t *s, uint16_t tx,
                                 unsigned frame_bits, int msb_first,
                                 lcd_common_state_t *st,
                                 const lcd_controller_ops_t *ops) {
    uint16_t mask = frame_mask(frame_bits);
    if (!st->frame_pending || !st->frame_selected ||
        !ssc_supported(p, frame_bits, msb_first, st->frame_is_data, ops)) {
        st->frame_pending = 0;
        return mask;
    }
    ops->ssc_consume(p, s, tx, frame_bits, msb_first, st->frame_is_data);
    st->frame_pending = 0;
    return mask;
}

void cemu_lcd_common_ssc_abort(lcd_common_state_t *st) {
    st->frame_pending = 0;
}

int cemu_lcd_common_flush_pending_frame(peripheral_t *p, soc_t *s,
                                   lcd_common_state_t *st,
                                   const lcd_controller_ops_t *ops) {
    /* A policy controller needs a real sweep boundary; run-end capture must
     * neither publish nor retain an unfinished candidate. */
    if (ops->close_transaction) {
        if (ops->discard_transaction) ops->discard_transaction(p);
        return 0;
    }
    if (!st->transaction_data_bytes) return 0;
    close_data_frame(p, st, s, 1, ops);
    return 1;
}

void cemu_lcd_common_note_data_byte(lcd_common_state_t *st) {
    st->transaction_data_bytes++;
}

void cemu_lcd_common_note_pixel(lcd_common_state_t *st, soc_t *s) {
    st->data_seq++;
    st->last_data_icount = live_icount(s);
}

int cemu_lcd_common_render_rgb(const lcd_common_state_t *common,
                          const void *state, int raw,
                          unsigned native_width, unsigned native_height,
                          unsigned scale, uint8_t *rgb, size_t rgb_len,
                          lcd_pixel_fn pixel) {
    if (!common || !state || !rgb || !scale || !pixel) return -1;
    if (!raw) {
        native_width = common->panel_width;
        native_height = common->panel_height;
    }
    size_t width = (size_t)native_width * scale;
    size_t height = (size_t)native_height * scale;
    if (!width || !height || height > SIZE_MAX / width / 3u ||
        rgb_len < width * height * 3u)
        return -1;
    for (unsigned y = 0; y < native_height; y++) {
        for (unsigned x = 0; x < native_width; x++) {
            uint8_t color[3];
            pixel(state, x, y, raw, color);
            for (unsigned sy = 0; sy < scale; sy++) {
                for (unsigned sx = 0; sx < scale; sx++) {
                    size_t at = (((size_t)y * scale + sy) * width +
                                 (size_t)x * scale + sx) * 3u;
                    memcpy(rgb + at, color, 3);
                }
            }
        }
    }
    return 0;
}
