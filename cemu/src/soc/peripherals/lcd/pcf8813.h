/* Philips PCF8813 102x(67+1) monochrome LCD controller. */
#ifndef CEMU_PERIPH_PCF8813_H
#define CEMU_PERIPH_PCF8813_H

#include <stddef.h>
#include <stdint.h>
#include "peripheral.h"
#include "devices.h"
#include "lcd_common.h"

#define PCF8813_WIDTH 102u
#define PCF8813_HEIGHT 68u
#define PCF8813_BANKS 10u
#define PCF8813_DDRAM_SIZE (PCF8813_WIDTH * PCF8813_BANKS)
#define PCF8813_LEGACY_STATE_SIZE 1232u

enum {
    PCF8813_DISPLAY_BLANK = 0,
    PCF8813_DISPLAY_ALL_ON = 1,
    PCF8813_DISPLAY_NORMAL = 2,
    PCF8813_DISPLAY_INVERSE = 3,
};

typedef struct {
    lcd_common_state_t common;

    /* Dense banks 0..8 followed by icon bank 10. */
    uint8_t ddram[PCF8813_DDRAM_SIZE];
    uint8_t x, y, x_max, y_max;
    uint8_t power_down, vertical, extended, mirror_x, mirror_y;
    uint8_t display_mode;
    uint8_t prs, power_control, temperature, hv_stages, bias, vop;
    uint8_t data_order, bottom_row_swap, normal_mode, partial_mask;
    uint8_t initial_row, ram_start_line;
    uint8_t pending_command;

    int status_requested;
    int status_active;
    int status_bit;
    uint8_t status_value;

    /* Schema 32--34 ended here; preserve its tail padding as a blob prefix. */
    uint8_t legacy_reserved[7];
    uint8_t presented_ddram[PCF8813_DDRAM_SIZE];
    uint64_t transaction_count;
    uint8_t transaction_started;
    uint8_t transaction_start_x, transaction_start_y;
    uint8_t transaction_vertical, transaction_sweep_member;
    uint8_t transaction_shape_valid;
    uint8_t transaction_x_max, transaction_y_max;
    uint8_t transaction_next_x, transaction_next_y;
    uint8_t sweep_active, sweep_next_y, sweep_start_x;
    uint8_t sweep_x_max, sweep_y_max, sweep_vertical;
    uint32_t sweep_data_bytes;
    lcd_sweep_tracker_t sweep;
} pcf8813_state_t;

_Static_assert(offsetof(pcf8813_state_t, presented_ddram) ==
               PCF8813_LEGACY_STATE_SIZE,
               "PCF8813 snapshot fields must remain append-only");

void cemu_pcf8813_periph_init(peripheral_t *p, pcf8813_state_t *st,
                         const device_lcd_config_t *cfg);

/* Hardware reset semantics. DDRAM and observability sequence counters survive. */
void cemu_pcf8813_reset(peripheral_t *p, soc_t *s);

/* Decoded-byte endpoint used by the serial parser and focused tests. */
void cemu_pcf8813_write_byte(peripheral_t *p, soc_t *s, int is_data, uint8_t value);

/* SSC0 slave lifecycle. Status reads use the board's GPIO serial path. */
void cemu_pcf8813_ssc_start(void *ctx, soc_t *s, uint16_t tx,
                       unsigned frame_bits, int msb_first);
uint16_t cemu_pcf8813_ssc_complete(void *ctx, soc_t *s, uint16_t tx,
                              unsigned frame_bits, int msb_first);
void cemu_pcf8813_ssc_abort(void *ctx, soc_t *s);

/* Close a selected data-bearing transaction at run end. Returns 1 if closed. */
int cemu_pcf8813_flush_pending_frame(peripheral_t *p, soc_t *s);

/* Render the visible LCD output into packed RGB at an integer nearest-neighbor scale. */
int cemu_pcf8813_render_rgb(const pcf8813_state_t *st, unsigned scale,
                       uint8_t *rgb, size_t rgb_len);

/* Render RAM pixels without applying power or display-mode state. */
int cemu_pcf8813_render_ddram_rgb(const pcf8813_state_t *st, unsigned scale,
                             uint8_t *rgb, size_t rgb_len);

#endif /* CEMU_PERIPH_PCF8813_H */
