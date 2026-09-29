/* Board LCD controller dispatch and model-neutral frame access. */
#ifndef CEMU_PERIPH_LCD_H
#define CEMU_PERIPH_LCD_H

#include <stddef.h>
#include <stdint.h>

#include "devices.h"
#include "s6b33bx.h"
#include "hm17cm256.h"
#include "hm17cm4096.h"
#include "pcf8833_4wire.h"
#include "pcf8813.h"
#include "peripheral.h"

#define LCD_MAX_WIDTH 132u
#define LCD_MAX_HEIGHT 162u
#define LCD_MAX_RGB_SIZE ((size_t)LCD_MAX_WIDTH * LCD_MAX_HEIGHT * 3u)

typedef union {
    s6b33bx_state_t s6b33bx;
    pcf8813_state_t pcf8813;
    hm17cm256_state_t hm17cm256;
    hm17cm4096_state_t hm17cm4096;
    pcf8833_4wire_state_t pcf8833_4wire;
} lcd_state_storage_t;

int cemu_lcd_periph_init(peripheral_t *p, lcd_state_storage_t *storage,
                    const device_lcd_config_t *cfg);

void cemu_lcd_ssc_start(void *ctx, soc_t *s, uint16_t tx,
                   unsigned frame_bits, int msb_first);
uint16_t cemu_lcd_ssc_complete(void *ctx, soc_t *s, uint16_t tx,
                          unsigned frame_bits, int msb_first);
void cemu_lcd_ssc_abort(void *ctx, soc_t *s);

void cemu_lcd_dimensions(const peripheral_t *p, int raw,
                    unsigned *width, unsigned *height);
uint64_t cemu_lcd_frame_sequence(const peripheral_t *p);
uint64_t cemu_lcd_last_frame_icount(const peripheral_t *p);
int cemu_lcd_render_rgb(const peripheral_t *p, int raw, unsigned scale,
                   uint8_t *rgb, size_t rgb_len);
int cemu_lcd_flush_pending_frame(peripheral_t *p, soc_t *s);
size_t cemu_lcd_state_size(const peripheral_t *p);
size_t cemu_lcd_legacy_state_size(const peripheral_t *p);
size_t cemu_lcd_snapshot_state_size(const peripheral_t *p, unsigned schema);
int cemu_lcd_restore_state(peripheral_t *p, soc_t *s, const uint8_t *state,
                       size_t state_size, int legacy);
int cemu_lcd_restore_snapshot_state(peripheral_t *p, soc_t *s,
                                    const uint8_t *state,
                                    size_t state_size, unsigned schema);
void cemu_lcd_prepare_restore(peripheral_t *p, soc_t *s);
void cemu_lcd_finish_restore(peripheral_t *p, soc_t *s);

#endif /* CEMU_PERIPH_LCD_H */
