/* PCF8833-compatible address-window presentation tests. */
#include <stdio.h>
#include <string.h>

#include "pcf8833_4wire.h"
#include "soc.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    soc_t soc;
    peripheral_t periph;
    pcf8833_4wire_state_t st;
    memset(&soc, 0, sizeof soc);
    device_lcd_config_t cfg = {
        .model = "pcf8833-4wire", .select_addr = 0xFFC4,
        .select_bit = 11, .select_active_low = 1,
        .dc_addr = 0xFFD0, .dc_bit = 6, .reset_capcom_channel = -1,
        .panel_width = 2, .panel_height = 1,
    };
    cemu_pcf8833_4wire_periph_init(&periph, &st, &cfg);
    st.window_start_x = st.x = 0;
    st.window_start_y = st.y = 0;
    st.end_x = 1;
    st.end_y = 0;
    st.ram_write = 1;
    st.sleep_out = 1;

    cemu_pcf8833_4wire_write_pixel(&periph, &soc, 0xF00);
    CHECK(st.gram[0] == 0xF00 && st.presented_gram[0] == 0);
    CHECK(st.common.frame_seq == 0 && st.sweep.active);
    cemu_pcf8833_4wire_write_pixel(&periph, &soc, 0x0F0);
    CHECK(st.presented_gram[0] == 0xF00);
    CHECK(st.presented_gram[1] == 0x0F0);
    CHECK(st.common.frame_seq == 1 && !st.sweep.active);

    cemu_pcf8833_4wire_write_pixel(&periph, &soc, 0x00F);
    CHECK(st.sweep.active && st.presented_gram[0] == 0xF00);
    cemu_pcf8833_4wire_reset(&periph, &soc);
    CHECK(!st.sweep.active && st.common.frame_seq == 1);

    /* SW20 programs 101x81 but writes only the 101x80 visible viewport. */
    cfg.panel_width = 101;
    cfg.panel_height = 80;
    cfg.panel_origin_x = 1;
    cemu_pcf8833_4wire_periph_init(&periph, &st, &cfg);
    st.window_start_x = st.x = 1;
    st.window_start_y = st.y = 0;
    st.end_x = 101;
    st.end_y = 80;
    st.ram_write = st.sleep_out = 1;
    for (unsigned i = 0; i < 101 * 80 - 1; i++)
        cemu_pcf8833_4wire_write_pixel(&periph, &soc, 0xF00);
    CHECK(st.common.frame_seq == 0 && st.sweep.active);
    CHECK(st.presented_gram[1] == 0);
    cemu_pcf8833_4wire_write_pixel(&periph, &soc, 0x0F0);
    CHECK(st.common.frame_seq == 1 && !st.sweep.active);
    CHECK(st.presented_gram[1] == 0xF00);
    CHECK(st.presented_gram[79 * 132 + 101] == 0x0F0);
    CHECK(st.x == 1 && st.y == 80 && st.end_y == 80);

    /* Retain the off-screen row and the controller's inclusive wrap. */
    for (unsigned i = 0; i < 101; i++)
        cemu_pcf8833_4wire_write_pixel(&periph, &soc, 0x00F);
    CHECK(st.x == 1 && st.y == 0);
    CHECK(st.gram[80 * 132 + 1] == 0x00F);

    /* Starting partway down the window does not complete a full viewport. */
    cemu_pcf8833_4wire_periph_init(&periph, &st, &cfg);
    st.window_start_x = st.x = 1;
    st.window_start_y = 0;
    st.y = 79;
    st.end_x = 101;
    st.end_y = 80;
    st.ram_write = 1;
    for (unsigned i = 0; i < 101; i++)
        cemu_pcf8833_4wire_write_pixel(&periph, &soc, 0xF00);
    CHECK(st.common.frame_seq == 0 && st.sweep.active);

    printf("\n2 tests, %d failed\n", failures);
    return failures ? 1 : 0;
}
