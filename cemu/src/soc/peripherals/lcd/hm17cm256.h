/* Hynix HM17CM256 128x82 256-color LCD controller. */
#ifndef CEMU_PERIPH_HM17CM256_H
#define CEMU_PERIPH_HM17CM256_H

#include <stddef.h>
#include <stdint.h>

#include "devices.h"
#include "hm17.h"
#include "lcd_common.h"
#include "peripheral.h"

#define HM17CM256_WIDTH 128u
#define HM17CM256_HEIGHT 82u
#define HM17CM256_GRAPHIC_HEIGHT 80u
#define HM17CM256_GRAM_PIXELS (HM17CM256_WIDTH * HM17CM256_HEIGHT)
#define HM17CM256_LEGACY_STATE_SIZE 10736u

typedef struct {
    lcd_common_state_t common;
    hm17_state_t hm17;

    uint8_t gram[HM17CM256_GRAM_PIXELS];
    uint8_t palette[3][8];
    uint8_t glsb;
    uint8_t extension_cs;
    uint8_t oscillator_rf, oscillator_ffl;
    uint8_t dummy_y;
    uint8_t pwm_mode_control;

    /* Schema 32--34 ended here. Keep the old tail padding explicit so all
     * publication state is appended and legacy blobs remain prefix copies. */
    uint8_t legacy_reserved[7];
    uint8_t presented_gram[HM17CM256_GRAM_PIXELS];
    uint64_t transaction_count;
    uint8_t transaction_started;
    uint8_t transaction_start_x, transaction_start_y;
    uint8_t transaction_increment_x, transaction_increment_y;
    uint8_t transaction_window, transaction_sweep_member;
    uint8_t transaction_shape_valid;
    uint8_t transaction_end_x, transaction_end_y;
    uint8_t transaction_next_x, transaction_next_y;
    uint8_t sweep_active, sweep_next_y, sweep_start_x;
    uint8_t sweep_increment_x, sweep_increment_y, sweep_window;
    uint8_t sweep_end_x, sweep_end_y;
    uint32_t sweep_row_bytes, sweep_data_bytes;
    lcd_sweep_tracker_t sweep;
} hm17cm256_state_t;

_Static_assert(offsetof(hm17cm256_state_t, presented_gram) ==
               HM17CM256_LEGACY_STATE_SIZE,
               "HM17CM256 snapshot fields must remain append-only");

void cemu_hm17cm256_periph_init(peripheral_t *p, hm17cm256_state_t *st,
                           const device_lcd_config_t *cfg);
void cemu_hm17cm256_reset(peripheral_t *p, soc_t *s);
void cemu_hm17cm256_write_byte(peripheral_t *p, soc_t *s,
                          int is_data, uint8_t value);

void cemu_hm17cm256_ssc_start(void *ctx, soc_t *s, uint16_t tx,
                         unsigned frame_bits, int msb_first);
uint16_t cemu_hm17cm256_ssc_complete(void *ctx, soc_t *s, uint16_t tx,
                                unsigned frame_bits, int msb_first);
void cemu_hm17cm256_ssc_abort(void *ctx, soc_t *s);

int cemu_hm17cm256_flush_pending_frame(peripheral_t *p, soc_t *s);
int cemu_hm17cm256_render_rgb(const hm17cm256_state_t *st, int raw,
                         unsigned scale, uint8_t *rgb, size_t rgb_len);

#endif /* CEMU_PERIPH_HM17CM256_H */
