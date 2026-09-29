/* Hynix HM17CM4096 128x162 RGB444 LCD controller. */
#ifndef CEMU_PERIPH_HM17CM4096_H
#define CEMU_PERIPH_HM17CM4096_H

#include <stddef.h>
#include <stdint.h>

#include "devices.h"
#include "hm17.h"
#include "lcd_common.h"
#include "peripheral.h"

#define HM17CM4096_WIDTH 128u
#define HM17CM4096_HEIGHT 162u
#define HM17CM4096_GRAM_PIXELS (HM17CM4096_WIDTH * HM17CM4096_HEIGHT)

typedef struct {
    lcd_common_state_t common;
    hm17_state_t hm17;

    uint16_t gram[HM17CM4096_GRAM_PIXELS];
    uint8_t palette[3][16];
    uint8_t color_256;
    uint8_t high_speed_write, absolute_12bit;
    uint8_t palette_upper, line_reverse, blink_type;
    uint8_t pwm_control;
    uint8_t pack[3];
    uint8_t pack_count;
    uint16_t presented_gram[HM17CM4096_GRAM_PIXELS];
    lcd_sweep_tracker_t sweep;
} hm17cm4096_state_t;

void cemu_hm17cm4096_periph_init(peripheral_t *p, hm17cm4096_state_t *st,
                            const device_lcd_config_t *cfg);
void cemu_hm17cm4096_reset(peripheral_t *p, soc_t *s);
void cemu_hm17cm4096_write_byte(peripheral_t *p, soc_t *s,
                           int is_data, uint8_t value);

void cemu_hm17cm4096_ssc_start(void *ctx, soc_t *s, uint16_t tx,
                          unsigned frame_bits, int msb_first);
uint16_t cemu_hm17cm4096_ssc_complete(void *ctx, soc_t *s, uint16_t tx,
                                 unsigned frame_bits, int msb_first);
void cemu_hm17cm4096_ssc_abort(void *ctx, soc_t *s);

int cemu_hm17cm4096_flush_pending_frame(peripheral_t *p, soc_t *s);
int cemu_hm17cm4096_render_rgb(const hm17cm4096_state_t *st, int raw,
                          unsigned scale, uint8_t *rgb, size_t rgb_len);

#endif /* CEMU_PERIPH_HM17CM4096_H */
