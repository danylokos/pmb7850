/* PCF8833-compatible 132x132 RGB444 controller with separate D/C. */
#ifndef CEMU_PERIPH_PCF8833_4WIRE_H
#define CEMU_PERIPH_PCF8833_4WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "devices.h"
#include "lcd_common.h"
#include "peripheral.h"

#define PCF8833_4WIRE_WIDTH 132u
#define PCF8833_4WIRE_HEIGHT 132u
#define PCF8833_4WIRE_GRAM_PIXELS \
    (PCF8833_4WIRE_WIDTH * PCF8833_4WIRE_HEIGHT)

typedef struct {
    lcd_common_state_t common;

    uint8_t x, y, window_start_x, window_start_y, end_x, end_y;
    uint8_t display_on, reverse;

    uint16_t gram[PCF8833_4WIRE_GRAM_PIXELS];
    uint8_t current_command;
    uint8_t parameter_index;
    uint8_t colmod;
    uint8_t madctl;
    uint8_t sleep_out;
    uint8_t partial_mode;
    uint8_t normal_mode;
    uint8_t idle_mode;
    uint8_t tearing_on;
    uint8_t booster_on;
    uint8_t ram_write;
    uint8_t all_pixels;
    uint8_t contrast;
    uint8_t partial_start, partial_end;
    uint8_t scroll_definition[3];
    uint8_t scroll_entry;
    uint8_t rgb_lut[20];
    uint8_t rgb_lut_count;
    uint32_t unknown_commands;
    uint16_t presented_gram[PCF8833_4WIRE_GRAM_PIXELS];
    lcd_sweep_tracker_t sweep;
} pcf8833_4wire_state_t;

void cemu_pcf8833_4wire_periph_init(peripheral_t *p,
                               pcf8833_4wire_state_t *st,
                               const device_lcd_config_t *cfg);
void cemu_pcf8833_4wire_reset(peripheral_t *p, soc_t *s);
void cemu_pcf8833_4wire_write_byte(peripheral_t *p, soc_t *s,
                              int is_data, uint8_t value);
void cemu_pcf8833_4wire_write_pixel(peripheral_t *p, soc_t *s, uint16_t value);

void cemu_pcf8833_4wire_ssc_start(void *ctx, soc_t *s, uint16_t tx,
                             unsigned frame_bits, int msb_first);
uint16_t cemu_pcf8833_4wire_ssc_complete(void *ctx, soc_t *s, uint16_t tx,
                                    unsigned frame_bits, int msb_first);
void cemu_pcf8833_4wire_ssc_abort(void *ctx, soc_t *s);

int cemu_pcf8833_4wire_flush_pending_frame(peripheral_t *p, soc_t *s);
int cemu_pcf8833_4wire_render_rgb(const pcf8833_4wire_state_t *st, int raw,
                             unsigned scale, uint8_t *rgb, size_t rgb_len);

#endif /* CEMU_PERIPH_PCF8833_4WIRE_H */
