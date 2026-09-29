/* Addressing behavior shared by the HM17-family controller models. */
#include "hm17.h"

void cemu_hm17_increment_address(hm17_state_t *st,
                            unsigned x_limit, unsigned y_limit) {
    if (st->window) {
        x_limit = st->end_x;
        y_limit = st->end_y;
    }
    unsigned x_start = st->window ? st->window_start_x : 0u;
    unsigned y_start = st->window ? st->window_start_y : 0u;

    if (st->increment_x) {
        if (st->x >= x_limit) {
            st->x = (uint8_t)x_start;
            if (st->increment_y)
                st->y = st->y >= y_limit ? (uint8_t)y_start
                                         : (uint8_t)(st->y + 1u);
        } else {
            st->x++;
        }
    } else if (st->increment_y) {
        st->y = st->y >= y_limit ? (uint8_t)y_start
                                 : (uint8_t)(st->y + 1u);
    }
}
