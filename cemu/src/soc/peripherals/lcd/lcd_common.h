/* Controller-neutral LCD wiring, transport, tracing, and rendering. */
#ifndef CEMU_PERIPH_LCD_COMMON_H
#define CEMU_PERIPH_LCD_COMMON_H

#include <stddef.h>
#include <stdint.h>

#include "devices.h"
#include "peripheral.h"
#include "cemu_event.h"

typedef struct {
    uint8_t gpio_shift, ssc_shift;
    unsigned gpio_shift_bits, ssc_shift_bits;
    int selected, select_known;
    int reset_asserted, reset_known;
    int gpio_clock_known, gpio_clock_level;
    int gpio_data_direction_known, gpio_data_is_output;
    int frame_pending, frame_selected, frame_is_data;

    uint32_t select_addr;
    int select_bit, select_active_low;
    uint32_t dc_addr;
    int dc_bit;
    uint32_t gpio_clock_addr;
    int gpio_clock_bit;
    uint32_t gpio_data_addr;
    uint32_t gpio_data_direction_addr;
    int gpio_data_bit;
    int reset_capcom_channel;
    int reset_capcom_idle_level;

    unsigned panel_width, panel_height;
    unsigned panel_origin_x, panel_origin_y;
    int panel_mirror_x, panel_mirror_y;

    uint64_t data_seq, frame_seq;
    uint64_t last_data_icount, last_frame_icount;
    uint32_t transaction_data_bytes;
    uint32_t unsupported_transfers;
} lcd_common_state_t;

/* Controller-neutral logical repaint state.  This is appended to controller
 * snapshots (rather than lcd_common_state_t) so the historical controller
 * prefixes remain byte-for-byte stable. */
typedef struct {
    uint64_t transaction_sequence;
    uint64_t transaction_frame_sequence;
    uint32_t data_bytes;
    uint32_t storage_writes;
    uint32_t mode;
    uint16_t origin_x, origin_y;
    uint16_t expected_x, expected_y;
    uint16_t start_x, start_y, end_x, end_y;
    uint16_t transaction_x, transaction_y;
    uint8_t active;
    uint8_t windowed;
    uint8_t pack_phase;
    uint8_t transaction_started;
} lcd_sweep_tracker_t;

typedef struct {
    unsigned x, y;
    unsigned next_x, next_y;
    unsigned start_x, start_y, end_x, end_y;
    uint32_t mode;
    unsigned pack_phase;
    unsigned storage_writes;
    unsigned data_bytes;
    int windowed;
    int terminal;
} lcd_sweep_write_t;

typedef struct {
    const char *select_detail;
    const char *reset_detail;
    const char *frame_detail;
    const char *unsupported_detail;
    int data_active_high;
    int select_reports_reset;
    int track_reset;

    void (*reset)(peripheral_t *p, soc_t *s);
    void (*clear_partial)(peripheral_t *p);
    void (*write_byte)(peripheral_t *p, soc_t *s,
                       int is_data, uint8_t value);
    int (*ssc_supported)(peripheral_t *p, unsigned frame_bits,
                         int msb_first, int is_data);
    void (*ssc_consume)(peripheral_t *p, soc_t *s, uint16_t tx,
                        unsigned frame_bits, int msb_first, int is_data);
    void (*select_changed)(peripheral_t *p, soc_t *s, int selected);
    void (*gpio_direction_changed)(peripheral_t *p, soc_t *s, int output);
    void (*gpio_input_falling_edge)(peripheral_t *p, soc_t *s);
    /* Optional presentation policy. Controllers that leave these unset keep
     * the historical one-presentation-per-data-transaction behavior. */
    void (*close_transaction)(peripheral_t *p, soc_t *s);
    void (*discard_transaction)(peripheral_t *p);
    void (*present)(peripheral_t *p);
} lcd_controller_ops_t;

typedef void (*lcd_trace_extra_fn)(cemu_native_trace_event_t *ev, const void *state);
typedef void (*lcd_pixel_fn)(const void *state, unsigned x, unsigned y,
                             int raw, uint8_t out[3]);

void cemu_lcd_common_init(lcd_common_state_t *st, const device_lcd_config_t *cfg,
                     unsigned default_width, unsigned default_height);
void cemu_lcd_common_reset_serial(lcd_common_state_t *st);

void cemu_lcd_common_tick(peripheral_t *p, soc_t *s, int n,
                     lcd_common_state_t *st,
                     const lcd_controller_ops_t *ops);
uint64_t cemu_lcd_common_next_event(peripheral_t *p, soc_t *s,
                               lcd_common_state_t *st,
                               const lcd_controller_ops_t *ops);
void cemu_lcd_common_advance_quiet(peripheral_t *p, soc_t *s, uint64_t ticks);

void cemu_lcd_common_ssc_start(peripheral_t *p, soc_t *s, uint16_t tx,
                          unsigned frame_bits, int msb_first,
                          lcd_common_state_t *st,
                          const lcd_controller_ops_t *ops);
uint16_t cemu_lcd_common_ssc_complete(peripheral_t *p, soc_t *s, uint16_t tx,
                                 unsigned frame_bits, int msb_first,
                                 lcd_common_state_t *st,
                                 const lcd_controller_ops_t *ops);
void cemu_lcd_common_ssc_abort(lcd_common_state_t *st);
int cemu_lcd_common_flush_pending_frame(peripheral_t *p, soc_t *s,
                                   lcd_common_state_t *st,
                                   const lcd_controller_ops_t *ops);

void cemu_lcd_common_note_data_byte(lcd_common_state_t *st);
void cemu_lcd_common_note_pixel(lcd_common_state_t *st, soc_t *s);
void cemu_lcd_common_publish_frame(lcd_common_state_t *st, soc_t *s,
                               uint32_t data_bytes,
                               const lcd_controller_ops_t *ops);
void cemu_lcd_sweep_reset(lcd_common_state_t *st,
                          lcd_sweep_tracker_t *tracker);
void cemu_lcd_sweep_prepare_write(peripheral_t *p, soc_t *s,
                                  lcd_common_state_t *st,
                                  lcd_sweep_tracker_t *tracker,
                                  const lcd_sweep_write_t *write,
                                  const lcd_controller_ops_t *ops);
void cemu_lcd_sweep_finish_write(peripheral_t *p, soc_t *s,
                                 lcd_common_state_t *st,
                                 lcd_sweep_tracker_t *tracker,
                                 const lcd_sweep_write_t *write,
                                 const lcd_controller_ops_t *ops);
/* Publish an evidenced complete sweep without changing controller addressing. */
void cemu_lcd_sweep_publish(peripheral_t *p, soc_t *s,
                            lcd_common_state_t *st,
                            lcd_sweep_tracker_t *tracker,
                            const lcd_controller_ops_t *ops,
                            const char *boundary);
void cemu_lcd_sweep_fail_open(peripheral_t *p, soc_t *s,
                              lcd_common_state_t *st,
                              lcd_sweep_tracker_t *tracker,
                              const lcd_controller_ops_t *ops);
void cemu_lcd_sweep_close_transaction(peripheral_t *p, soc_t *s,
                                      lcd_common_state_t *st,
                                      lcd_sweep_tracker_t *tracker,
                                      const lcd_controller_ops_t *ops,
                                      const char *detail);
void cemu_lcd_sweep_discard_transaction(lcd_common_state_t *st,
                                        lcd_sweep_tracker_t *tracker);
void cemu_lcd_common_emit_transaction(lcd_common_state_t *st, soc_t *s,
                                  uint64_t sequence, uint32_t data_bytes,
                                  unsigned x, unsigned y,
                                  const char *disposition,
                                  const char *detail);

void cemu_lcd_common_emit_reset(lcd_common_state_t *st, soc_t *s, int asserted,
                           const lcd_controller_ops_t *ops);
void cemu_lcd_common_emit_command(lcd_common_state_t *st, soc_t *s,
                             const void *state, uint8_t value, int known,
                             const char *name, lcd_trace_extra_fn extra);
void cemu_lcd_common_emit_data(lcd_common_state_t *st, soc_t *s,
                          const void *state, uint32_t value, unsigned size,
                          unsigned x, unsigned y, const char *detail,
                          lcd_trace_extra_fn extra);
void cemu_lcd_common_emit_unsupported(lcd_common_state_t *st, soc_t *s,
                                 unsigned frame_bits, int msb_first,
                                 const lcd_controller_ops_t *ops);

int cemu_lcd_common_render_rgb(const lcd_common_state_t *common,
                          const void *state, int raw,
                          unsigned native_width, unsigned native_height,
                          unsigned scale, uint8_t *rgb, size_t rgb_len,
                          lcd_pixel_fn pixel);

#endif /* CEMU_PERIPH_LCD_COMMON_H */
