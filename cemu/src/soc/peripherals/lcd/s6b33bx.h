/*
 * S6B33Bx-compatible LCD transport.
 *
 * The family name reflects protocol compatibility rather than an exact die
 * identification. The S6B33B2 reference documents GRAM X 0..161 and Y 0..131,
 * but this calibrated state stores only a 130x130 visible panel subset. It
 * models X/Y windows and RGB565 writes, not full reset/power state, the
 * complete command set, 3-pin/parallel modes, or status/readback.
 */
#ifndef CEMU_PERIPH_S6B33BX_H
#define CEMU_PERIPH_S6B33BX_H

#include <stddef.h>
#include <stdint.h>

#include "devices.h"
#include "lcd_common.h"
#include "peripheral.h"

#define S6B33BX_WIDTH 130u
#define S6B33BX_HEIGHT 130u
#define S6B33BX_GRAM_PIXELS (S6B33BX_WIDTH * S6B33BX_HEIGHT)

typedef struct {
    lcd_common_state_t common;
    uint16_t gram[S6B33BX_GRAM_PIXELS];
    uint32_t write_index;
    uint32_t command_count;
    uint32_t unknown_commands;
    /* Visible panel coordinates; controller X/Y map to panel Y/X. */
    uint8_t window_start_x, window_start_y;
    uint8_t window_end_x, window_end_y;
    uint8_t pending_window_command;
    uint8_t pending_window_index;
    uint8_t pending_window_start;
    uint8_t last_command;
    uint16_t presented_gram[S6B33BX_GRAM_PIXELS];
    lcd_sweep_tracker_t sweep;
} s6b33bx_state_t;

void cemu_s6b33bx_periph_init(peripheral_t *p, s6b33bx_state_t *st,
                         const device_lcd_config_t *cfg);
void cemu_s6b33bx_write_command(peripheral_t *p, soc_t *s, uint8_t value);
void cemu_s6b33bx_write_pixel(peripheral_t *p, soc_t *s, uint16_t value);

void cemu_s6b33bx_ssc_start(void *ctx, soc_t *s, uint16_t tx,
                       unsigned frame_bits, int msb_first);
uint16_t cemu_s6b33bx_ssc_complete(void *ctx, soc_t *s, uint16_t tx,
                              unsigned frame_bits, int msb_first);
void cemu_s6b33bx_ssc_abort(void *ctx, soc_t *s);

int cemu_s6b33bx_flush_pending_frame(peripheral_t *p, soc_t *s);
int cemu_s6b33bx_render_rgb(const s6b33bx_state_t *st, int raw,
                       unsigned scale, uint8_t *rgb, size_t rgb_len);

#endif /* CEMU_PERIPH_S6B33BX_H */
