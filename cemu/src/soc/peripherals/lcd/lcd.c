/* Board LCD controller dispatch and model-neutral frame access. */
#include <string.h>

#include "lcd.h"
#include "soc.h"

int cemu_lcd_periph_init(peripheral_t *p, lcd_state_storage_t *storage,
                    const device_lcd_config_t *cfg) {
    if (!p || !storage || !cfg || !cfg->model) return 0;
    if (!strcmp(cfg->model, "s6b33bx")) {
        cemu_s6b33bx_periph_init(p, &storage->s6b33bx, cfg);
        return 1;
    }
    if (!strcmp(cfg->model, "pcf8813")) {
        cemu_pcf8813_periph_init(p, &storage->pcf8813, cfg);
        return 1;
    }
    if (!strcmp(cfg->model, "hm17cm256")) {
        cemu_hm17cm256_periph_init(p, &storage->hm17cm256, cfg);
        return 1;
    }
    if (!strcmp(cfg->model, "hm17cm4096")) {
        cemu_hm17cm4096_periph_init(p, &storage->hm17cm4096, cfg);
        return 1;
    }
    if (!strcmp(cfg->model, "pcf8833-4wire")) {
        cemu_pcf8833_4wire_periph_init(p, &storage->pcf8833_4wire, cfg);
        return 1;
    }
    return 0;
}

void cemu_lcd_ssc_start(void *ctx, soc_t *s, uint16_t tx,
                   unsigned frame_bits, int msb_first) {
    peripheral_t *p = (peripheral_t *)ctx;
    if (p && !strcmp(p->model, "s6b33bx"))
        cemu_s6b33bx_ssc_start(ctx, s, tx, frame_bits, msb_first);
    else if (p && !strcmp(p->model, "pcf8813"))
        cemu_pcf8813_ssc_start(ctx, s, tx, frame_bits, msb_first);
    else if (p && !strcmp(p->model, "hm17cm256"))
        cemu_hm17cm256_ssc_start(ctx, s, tx, frame_bits, msb_first);
    else if (p && !strcmp(p->model, "hm17cm4096"))
        cemu_hm17cm4096_ssc_start(ctx, s, tx, frame_bits, msb_first);
    else if (p && !strcmp(p->model, "pcf8833-4wire"))
        cemu_pcf8833_4wire_ssc_start(ctx, s, tx, frame_bits, msb_first);
}

uint16_t cemu_lcd_ssc_complete(void *ctx, soc_t *s, uint16_t tx,
                          unsigned frame_bits, int msb_first) {
    peripheral_t *p = (peripheral_t *)ctx;
    if (p && !strcmp(p->model, "s6b33bx"))
        return cemu_s6b33bx_ssc_complete(ctx, s, tx, frame_bits, msb_first);
    if (p && !strcmp(p->model, "pcf8813"))
        return cemu_pcf8813_ssc_complete(ctx, s, tx, frame_bits, msb_first);
    if (p && !strcmp(p->model, "hm17cm256"))
        return cemu_hm17cm256_ssc_complete(ctx, s, tx, frame_bits, msb_first);
    if (p && !strcmp(p->model, "hm17cm4096"))
        return cemu_hm17cm4096_ssc_complete(ctx, s, tx, frame_bits, msb_first);
    if (p && !strcmp(p->model, "pcf8833-4wire"))
        return cemu_pcf8833_4wire_ssc_complete(ctx, s, tx,
                                          frame_bits, msb_first);
    return frame_bits >= 16 ? 0xFFFFu : (uint16_t)((1u << frame_bits) - 1u);
}

void cemu_lcd_ssc_abort(void *ctx, soc_t *s) {
    peripheral_t *p = (peripheral_t *)ctx;
    if (p && !strcmp(p->model, "s6b33bx"))
        cemu_s6b33bx_ssc_abort(ctx, s);
    else if (p && !strcmp(p->model, "pcf8813")) cemu_pcf8813_ssc_abort(ctx, s);
    else if (p && !strcmp(p->model, "hm17cm256"))
        cemu_hm17cm256_ssc_abort(ctx, s);
    else if (p && !strcmp(p->model, "hm17cm4096"))
        cemu_hm17cm4096_ssc_abort(ctx, s);
    else if (p && !strcmp(p->model, "pcf8833-4wire"))
        cemu_pcf8833_4wire_ssc_abort(ctx, s);
}

void cemu_lcd_dimensions(const peripheral_t *p, int raw,
                    unsigned *width, unsigned *height) {
    (void)raw;
    unsigned w = 0, h = 0;
    if (p && !strcmp(p->model, "s6b33bx")) {
        const s6b33bx_state_t *st = (const s6b33bx_state_t *)p->state;
        w = raw ? S6B33BX_WIDTH : st->common.panel_width;
        h = raw ? S6B33BX_HEIGHT : st->common.panel_height;
    } else if (p && !strcmp(p->model, "pcf8813")) {
        const pcf8813_state_t *st = (const pcf8813_state_t *)p->state;
        w = raw ? PCF8813_WIDTH : st->common.panel_width;
        h = raw ? PCF8813_HEIGHT : st->common.panel_height;
    } else if (p && !strcmp(p->model, "hm17cm256")) {
        const hm17cm256_state_t *st = (const hm17cm256_state_t *)p->state;
        w = raw ? HM17CM256_WIDTH : st->common.panel_width;
        h = raw ? HM17CM256_HEIGHT : st->common.panel_height;
    } else if (p && !strcmp(p->model, "hm17cm4096")) {
        const hm17cm4096_state_t *st = (const hm17cm4096_state_t *)p->state;
        w = raw ? HM17CM4096_WIDTH : st->common.panel_width;
        h = raw ? HM17CM4096_HEIGHT : st->common.panel_height;
    } else if (p && !strcmp(p->model, "pcf8833-4wire")) {
        const pcf8833_4wire_state_t *st =
            (const pcf8833_4wire_state_t *)p->state;
        w = raw ? PCF8833_4WIRE_WIDTH : st->common.panel_width;
        h = raw ? PCF8833_4WIRE_HEIGHT : st->common.panel_height;
    }
    if (width) *width = w;
    if (height) *height = h;
}

uint64_t cemu_lcd_frame_sequence(const peripheral_t *p) {
    if (p && !strcmp(p->model, "s6b33bx"))
        return ((const s6b33bx_state_t *)p->state)->common.frame_seq;
    if (p && !strcmp(p->model, "pcf8813"))
        return ((const pcf8813_state_t *)p->state)->common.frame_seq;
    if (p && !strcmp(p->model, "hm17cm256"))
        return ((const hm17cm256_state_t *)p->state)->common.frame_seq;
    if (p && !strcmp(p->model, "hm17cm4096"))
        return ((const hm17cm4096_state_t *)p->state)->common.frame_seq;
    if (p && !strcmp(p->model, "pcf8833-4wire"))
        return ((const pcf8833_4wire_state_t *)p->state)->common.frame_seq;
    return 0;
}

uint64_t cemu_lcd_last_frame_icount(const peripheral_t *p) {
    if (p && !strcmp(p->model, "s6b33bx"))
        return ((const s6b33bx_state_t *)p->state)
            ->common.last_frame_icount;
    if (p && !strcmp(p->model, "pcf8813"))
        return ((const pcf8813_state_t *)p->state)->common.last_frame_icount;
    if (p && !strcmp(p->model, "hm17cm256"))
        return ((const hm17cm256_state_t *)p->state)->common.last_frame_icount;
    if (p && !strcmp(p->model, "hm17cm4096"))
        return ((const hm17cm4096_state_t *)p->state)->common.last_frame_icount;
    if (p && !strcmp(p->model, "pcf8833-4wire"))
        return ((const pcf8833_4wire_state_t *)p->state)
            ->common.last_frame_icount;
    return 0;
}

int cemu_lcd_render_rgb(const peripheral_t *p, int raw, unsigned scale,
                   uint8_t *rgb, size_t rgb_len) {
    if (p && !strcmp(p->model, "s6b33bx"))
        return cemu_s6b33bx_render_rgb((const s6b33bx_state_t *)p->state,
                                    raw, scale, rgb, rgb_len);
    if (p && !strcmp(p->model, "pcf8813")) {
        const pcf8813_state_t *st = (const pcf8813_state_t *)p->state;
        return raw ? cemu_pcf8813_render_ddram_rgb(st, scale, rgb, rgb_len)
                   : cemu_pcf8813_render_rgb(st, scale, rgb, rgb_len);
    }
    if (p && !strcmp(p->model, "hm17cm256"))
        return cemu_hm17cm256_render_rgb((const hm17cm256_state_t *)p->state,
                                   raw, scale, rgb, rgb_len);
    if (p && !strcmp(p->model, "hm17cm4096"))
        return cemu_hm17cm4096_render_rgb((const hm17cm4096_state_t *)p->state,
                                    raw, scale, rgb, rgb_len);
    if (p && !strcmp(p->model, "pcf8833-4wire"))
        return cemu_pcf8833_4wire_render_rgb(
            (const pcf8833_4wire_state_t *)p->state,
            raw, scale, rgb, rgb_len);
    return -1;
}

int cemu_lcd_flush_pending_frame(peripheral_t *p, soc_t *s) {
    if (p && !strcmp(p->model, "s6b33bx"))
        return cemu_s6b33bx_flush_pending_frame(p, s);
    if (p && !strcmp(p->model, "pcf8813"))
        return cemu_pcf8813_flush_pending_frame(p, s);
    if (p && !strcmp(p->model, "hm17cm256"))
        return cemu_hm17cm256_flush_pending_frame(p, s);
    if (p && !strcmp(p->model, "hm17cm4096"))
        return cemu_hm17cm4096_flush_pending_frame(p, s);
    if (p && !strcmp(p->model, "pcf8833-4wire"))
        return cemu_pcf8833_4wire_flush_pending_frame(p, s);
    return 0;
}

size_t cemu_lcd_state_size(const peripheral_t *p) {
    if (p && !strcmp(p->model, "s6b33bx"))
        return sizeof(s6b33bx_state_t);
    if (p && !strcmp(p->model, "pcf8813")) return sizeof(pcf8813_state_t);
    if (p && !strcmp(p->model, "hm17cm256"))
        return sizeof(hm17cm256_state_t);
    if (p && !strcmp(p->model, "hm17cm4096"))
        return sizeof(hm17cm4096_state_t);
    if (p && !strcmp(p->model, "pcf8833-4wire"))
        return sizeof(pcf8833_4wire_state_t);
    return 0;
}

size_t cemu_lcd_legacy_state_size(const peripheral_t *p) {
    if (p && !strcmp(p->model, "pcf8813"))
        return PCF8813_LEGACY_STATE_SIZE;
    if (p && !strcmp(p->model, "hm17cm256"))
        return HM17CM256_LEGACY_STATE_SIZE;
    if (p && !strcmp(p->model, "hm17cm4096"))
        return offsetof(hm17cm4096_state_t, presented_gram);
    if (p && !strcmp(p->model, "pcf8833-4wire"))
        return offsetof(pcf8833_4wire_state_t, presented_gram);
    if (p && !strcmp(p->model, "s6b33bx"))
        return offsetof(s6b33bx_state_t, presented_gram);
    return cemu_lcd_state_size(p);
}

size_t cemu_lcd_snapshot_state_size(const peripheral_t *p, unsigned schema) {
    if (schema >= 38) return cemu_lcd_state_size(p);
    if (schema < 35) {
        if (p && !strcmp(p->model, "pcf8813"))
            return PCF8813_LEGACY_STATE_SIZE;
        if (p && !strcmp(p->model, "hm17cm256"))
            return HM17CM256_LEGACY_STATE_SIZE;
        if (p && !strcmp(p->model, "hm17cm4096"))
            return offsetof(hm17cm4096_state_t, presented_gram);
        if (p && !strcmp(p->model, "pcf8833-4wire"))
            return offsetof(pcf8833_4wire_state_t, presented_gram);
        if (p && !strcmp(p->model, "s6b33bx"))
            return offsetof(s6b33bx_state_t, presented_gram);
        return cemu_lcd_state_size(p);
    }
    if (p && !strcmp(p->model, "pcf8813"))
        return offsetof(pcf8813_state_t, sweep);
    if (p && !strcmp(p->model, "hm17cm256"))
        return offsetof(hm17cm256_state_t, sweep);
    if (p && !strcmp(p->model, "hm17cm4096"))
        return offsetof(hm17cm4096_state_t, presented_gram);
    if (p && !strcmp(p->model, "pcf8833-4wire"))
        return offsetof(pcf8833_4wire_state_t, presented_gram);
    if (p && !strcmp(p->model, "s6b33bx"))
        return offsetof(s6b33bx_state_t, presented_gram);
    return cemu_lcd_state_size(p);
}

static void migrate_old_sweep(peripheral_t *p, unsigned schema) {
    if (!strcmp(p->model, "pcf8813")) {
        pcf8813_state_t *st = p->state;
        if (schema >= 35 && st->sweep_active && st->sweep_data_bytes &&
            st->sweep_start_x <= st->sweep_x_max &&
            st->sweep_next_y <= st->sweep_y_max) {
            st->sweep.active = 1;
            st->sweep.origin_x = st->sweep.start_x = st->sweep_start_x;
            st->sweep.origin_y = st->sweep.start_y = 0;
            st->sweep.expected_x = st->sweep_start_x;
            st->sweep.expected_y = st->sweep_next_y;
            st->sweep.end_x = st->sweep_x_max;
            st->sweep.end_y = (uint16_t)((st->common.panel_origin_y +
                st->common.panel_height - 1u) / 8u);
            st->sweep.mode = st->sweep_vertical;
            st->sweep.data_bytes = st->sweep_data_bytes;
            st->sweep.storage_writes = st->sweep_data_bytes;
        }
        st->sweep.transaction_sequence = st->transaction_count;
    } else if (!strcmp(p->model, "hm17cm256")) {
        hm17cm256_state_t *st = p->state;
        if (schema >= 35 && st->sweep_active && st->sweep_data_bytes &&
            st->sweep_start_x <= st->sweep_end_x &&
            st->sweep_next_y <= st->sweep_end_y) {
            st->sweep.active = 1;
            st->sweep.origin_x = st->sweep.start_x = st->sweep_start_x;
            st->sweep.origin_y = st->sweep.start_y =
                st->common.panel_origin_y;
            st->sweep.expected_x = st->sweep_start_x;
            st->sweep.expected_y = st->sweep_next_y;
            st->sweep.end_x = st->sweep_end_x;
            st->sweep.end_y = (uint16_t)(st->common.panel_origin_y +
                                         st->common.panel_height - 1u);
            st->sweep.mode = st->sweep_increment_x |
                (st->sweep_increment_y << 1) | (st->sweep_window << 2);
            st->sweep.windowed = st->sweep_window;
            st->sweep.data_bytes = st->sweep_data_bytes;
            st->sweep.storage_writes = st->sweep_data_bytes;
        }
        st->sweep.transaction_sequence = st->transaction_count;
    }
}

int cemu_lcd_restore_snapshot_state(peripheral_t *p, soc_t *s,
                                    const uint8_t *state,
                                    size_t state_size, unsigned schema) {
    size_t expected = cemu_lcd_state_size(p);
    size_t accepted = cemu_lcd_snapshot_state_size(p, schema);
    if (!p || !state || state_size != accepted) return 0;
    cemu_lcd_prepare_restore(p, s);
    memset(p->state, 0, expected);
    memcpy(p->state, state, state_size);
    if (!strcmp(p->model, "pcf8813")) {
        pcf8813_state_t *st = p->state;
        if (schema < 35)
            memcpy(st->presented_ddram, st->ddram,
                   sizeof st->presented_ddram);
    } else if (!strcmp(p->model, "hm17cm256")) {
        hm17cm256_state_t *st = p->state;
        if (schema < 35)
            memcpy(st->presented_gram, st->gram,
                   sizeof st->presented_gram);
    } else if (!strcmp(p->model, "hm17cm4096")) {
        hm17cm4096_state_t *st = p->state;
        if (schema < 38)
            memcpy(st->presented_gram, st->gram,
                   sizeof st->presented_gram);
    } else if (!strcmp(p->model, "pcf8833-4wire")) {
        pcf8833_4wire_state_t *st = p->state;
        if (schema < 38)
            memcpy(st->presented_gram, st->gram,
                   sizeof st->presented_gram);
    } else if (!strcmp(p->model, "s6b33bx")) {
        s6b33bx_state_t *st = p->state;
        if (schema < 38)
            memcpy(st->presented_gram, st->gram,
                   sizeof st->presented_gram);
    }
    if (schema < 38) migrate_old_sweep(p, schema);
    cemu_lcd_finish_restore(p, s);
    return 1;
}

int cemu_lcd_restore_state(peripheral_t *p, soc_t *s, const uint8_t *state,
                       size_t state_size, int legacy) {
    return cemu_lcd_restore_snapshot_state(p, s, state, state_size,
                                           legacy ? 34u : 38u);
}

void cemu_lcd_prepare_restore(peripheral_t *p, soc_t *s) {
    if (p && s && !strcmp(p->model, "pcf8813")) {
        pcf8813_state_t *st = (pcf8813_state_t *)p->state;
        cemu_soc_port_input_release(s, SOC_PORT_P7, st->common.gpio_data_bit);
    }
}

void cemu_lcd_finish_restore(peripheral_t *p, soc_t *s) {
    if (p && s && !strcmp(p->model, "pcf8813")) {
        pcf8813_state_t *st = (pcf8813_state_t *)p->state;
        if (st->status_active && st->status_bit >= 0)
            cemu_soc_port_input_level(s, SOC_PORT_P7, st->common.gpio_data_bit,
                                 (st->status_value >> st->status_bit) & 1u);
    }
}
