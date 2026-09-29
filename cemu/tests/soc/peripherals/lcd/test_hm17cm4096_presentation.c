/* HM17CM4096 address-sweep presentation tests. */
#include <stdio.h>
#include <string.h>

#include "hm17cm4096.h"
#include "soc.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    soc_t soc;
    peripheral_t periph;
    hm17cm4096_state_t st;
    memset(&soc, 0, sizeof soc);
    device_lcd_config_t cfg = {
        .model = "hm17cm4096", .select_addr = 0xFFC4,
        .select_bit = 11, .select_active_low = 1,
        .dc_addr = 0xFFD0, .dc_bit = 6, .reset_capcom_channel = -1,
        .panel_width = 2, .panel_height = 1,
    };
    cemu_hm17cm4096_periph_init(&periph, &st, &cfg);
    st.hm17.window = 1;
    st.hm17.increment_x = 1;
    st.hm17.increment_y = 1;
    st.hm17.window_start_x = st.hm17.window_start_y = 0;
    st.hm17.end_x = 3;
    st.hm17.end_y = 0;
    st.absolute_12bit = 1;
    st.hm17.display_on = 1;

    cemu_hm17cm4096_write_byte(&periph, &soc, 1, 0x0F);
    cemu_hm17cm4096_write_byte(&periph, &soc, 1, 0x00);
    CHECK(st.gram[0] == 0xF00 && st.presented_gram[0] == 0);
    CHECK(st.common.frame_seq == 0 && st.sweep.active);
    cemu_hm17cm4096_write_byte(&periph, &soc, 1, 0x00);
    CHECK(st.pack_count == 1 && st.common.frame_seq == 0);
    cemu_hm17cm4096_write_byte(&periph, &soc, 1, 0xF0);
    CHECK(st.gram[1] == 0x0F0 && st.presented_gram[1] == 0x0F0);
    CHECK(st.presented_gram[0] == 0xF00);
    CHECK(st.common.frame_seq == 1 && !st.sweep.active && !st.pack_count);

    cemu_hm17cm4096_write_byte(&periph, &soc, 1, 0x12);
    CHECK(st.sweep.active && st.presented_gram[0] == 0xF00);
    cemu_hm17cm4096_reset(&periph, &soc);
    CHECK(!st.sweep.active && st.common.frame_seq == 1);

    printf("\n1 tests, %d failed\n", failures);
    return failures ? 1 : 0;
}
