/* Register state and helpers shared only by Hynix HM17-family controllers. */
#ifndef CEMU_PERIPH_HM17_H
#define CEMU_PERIPH_HM17_H

#include <stdint.h>

typedef struct {
    uint8_t x, y, window_start_x, window_start_y, end_x, end_y;
    uint8_t start_line, n_line, line_start, line_end;
    uint8_t re, scan_start, duty, boost, bias, electric_volume;
    uint8_t display_on, all_on, reverse, common_shift;
    uint8_t n_line_enable, swap, reflect;
    uint8_t window, aim, increment_x, increment_y;
    uint8_t amp_on, halt, dc_on, discharge;
    uint8_t monochrome, fixed_pwm, clock_select, word_length_16;
} hm17_state_t;

void cemu_hm17_increment_address(hm17_state_t *st,
                            unsigned x_limit, unsigned y_limit);

#endif /* CEMU_PERIPH_HM17_H */
