/* S6B33Bx transport, isolation, rendering, and state tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "s6b33bx.h"
#include "debugger.h"
#include "lcd.h"
#include "soc.h"
#include "state_digest.h"
#include "synth.h"

static int g_fail, g_total_fail, g_total_run;
#define CHECK(cond) do { if (!(cond)) { g_fail++; \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define P3          0xFFC4u
#define P7          0xFFD0u
#define P8          0xFFD4u
#define MAIN_CS_BIT 11
#define SUB_CS_BIT  10
#define DISP_RS_BIT 13
#define SSC0TB      0xF0B0u
#define SSC0BR      0xF0B4u
#define SSC0CON     0xFFB2u
#define SSC_EN      (1u << 15)
#define SSC_MS      (1u << 14)
#define SSC_HB      (1u << 4)
#define FLEN        (8u * 1024u * 1024u)

static const device_config_t TEST_DEVICE = {
    .name = "s6b33bx-test",
    .flash = {
        .chips = {{
            .name = "flash",
            .model = "m58lw064d",
            .chip_size = FLEN,
        }},
        .nchips = 1,
        .windows = {{
            .cpu_base = 0,
            .cpu_size = FLEN,
            .chip_index = 0,
            .chip_base = 0,
            .mirror_period = FLEN,
            .command_visible = 1,
        }},
        .nwindows = 1,
    },
    .lcd = {
        .model = "s6b33bx",
        .select_addr = P3,
        .select_bit = MAIN_CS_BIT,
        .select_active_low = 1,
        .dc_addr = P8,
        .dc_bit = DISP_RS_BIT,
        .gpio_clock_addr = P7,
        .gpio_clock_bit = 5,
        .gpio_data_addr = P7,
        .gpio_data_bit = 7,
        .reset_capcom_channel = -1,
        .panel_width = S6B33BX_WIDTH,
        .panel_height = S6B33BX_HEIGHT,
    },
};

static soc_t S;
static uint8_t *flash;
static unsigned configured_bits;

static void setup(void) {
    if (!flash) {
        flash = malloc(FLEN);
        CHECK(flash != NULL);
        memset(flash, 0xFF, FLEN);
    }
    cemu_soc_init(&S, flash, FLEN, &TEST_DEVICE, synth_defaults(), 0);
}

static s6b33bx_state_t *lcd(void) {
    CHECK(S.lcd_periph != NULL);
    return S.lcd_periph ? S.lcd_periph->state : NULL;
}

static void set_pin(uint32_t addr, int bit, int level) {
    uint16_t value = memory_controller_sfr_get(&S.memory, addr);
    if (level) value |= (uint16_t)(1u << bit);
    else value &= (uint16_t)~(1u << bit);
    bus_write16(&S.bus, addr, value);
}

static void set_main_selected(int selected) {
    set_pin(P3, MAIN_CS_BIT, !selected);
    cemu_soc_tick(&S, 1);
}

static void set_sub_selected(int selected) {
    set_pin(P7, SUB_CS_BIT, !selected);
    cemu_soc_tick(&S, 1);
}

static void set_data_mode(int data) {
    set_pin(P8, DISP_RS_BIT, data);
}

static void configure_ssc(unsigned bits, int msb_first) {
    configured_bits = bits;
    bus_write16(&S.bus, SSC0BR, 0);
    uint16_t config = (uint16_t)(SSC_EN | SSC_MS |
                                 ((bits - 1u) & 0x0Fu));
    if (msb_first) config |= SSC_HB;
    bus_write16(&S.bus, SSC0CON, 0);
    bus_write16(&S.bus, SSC0CON, config);
}

static void send_ssc(uint16_t value) {
    bus_write16(&S.bus, SSC0TB, value);
    cemu_soc_tick(&S, (int)(2u * configured_bits));
}

static void test_registration_and_geometry(void) {
    setup();
    s6b33bx_state_t *st = lcd();
    CHECK(!strcmp(S.lcd_periph->model, "s6b33bx"));
    CHECK(st->common.select_addr == P3);
    CHECK(st->common.select_bit == MAIN_CS_BIT);
    CHECK(st->common.select_active_low);
    CHECK(st->common.dc_addr == P8 && st->common.dc_bit == DISP_RS_BIT);
    CHECK(st->common.panel_width == 130);
    CHECK(st->common.panel_height == 130);
    CHECK(st->window_start_x == 0 && st->window_end_x == 129);
    CHECK(st->window_start_y == 0 && st->window_end_y == 129);

    unsigned width = 0, height = 0;
    cemu_lcd_dimensions(S.lcd_periph, 0, &width, &height);
    CHECK(width == 130 && height == 130);
    cemu_lcd_dimensions(S.lcd_periph, 1, &width, &height);
    CHECK(width == 130 && height == 130);
    cemu_soc_free(&S);
}

static void test_main_select_rs_and_sub_isolation(void) {
    setup();
    s6b33bx_state_t *st = lcd();
    set_main_selected(0);
    set_sub_selected(0);
    configure_ssc(8, 1);
    set_data_mode(0);

    set_sub_selected(1);
    send_ssc(0x2C);
    set_sub_selected(0);
    CHECK(st->command_count == 0);

    set_main_selected(1);
    send_ssc(0x2C);
    set_main_selected(0);
    CHECK(st->command_count == 1);
    CHECK(st->last_command == 0x2C);
    CHECK(st->unknown_commands == 0);
    CHECK(st->common.unsupported_transfers == 0);
    cemu_soc_free(&S);
}

static void test_measured_commands_and_transfer_validation(void) {
    static const uint8_t commands[] = {
        0x2C,0x02,0x01,0x10,0x35,0x20,0x01,0x22,
        0x01,0x24,0x05,0x28,0x00,0x2A,0xAF,0x32,
        0x01,0x34,0x82,0x40,0x00,0x30,0x01,
        0x43,0x00,0x81,0x42,0x00,0x81,
        0x43,0x01,0x80,0x42,0x01,0x80,
        0x2C,0x51,0x26,0x0F,0x2A,0xAF,
    };
    setup();
    s6b33bx_state_t *st = lcd();
    for (size_t i = 0; i < sizeof commands; i++)
        cemu_s6b33bx_write_command(S.lcd_periph, &S, commands[i]);
    CHECK(st->command_count == sizeof commands);
    CHECK(st->unknown_commands == 0);
    cemu_s6b33bx_write_command(S.lcd_periph, &S, 0xFF);
    CHECK(st->unknown_commands == 1);

    set_main_selected(0);
    set_data_mode(1);
    configure_ssc(8, 1);
    set_main_selected(1);
    send_ssc(0x5A);
    CHECK(st->common.unsupported_transfers == 1);
    configure_ssc(16, 0);
    send_ssc(0xF800);
    CHECK(st->common.unsupported_transfers == 2);
    set_main_selected(0);
    cemu_soc_free(&S);
}

static void test_rgb565_renderer_calibration_and_wrap(void) {
    setup();
    s6b33bx_state_t *st = lcd();
    cemu_s6b33bx_write_pixel(S.lcd_periph, &S, 0xF800);
    cemu_s6b33bx_write_pixel(S.lcd_periph, &S, 0x07E0);
    cemu_s6b33bx_write_pixel(S.lcd_periph, &S, 0x001F);
    CHECK(st->gram[0] == 0xF800);
    CHECK(st->gram[1] == 0x07E0);
    CHECK(st->gram[2] == 0x001F);
    CHECK(st->write_index == 3);

    uint8_t rgb[S6B33BX_GRAM_PIXELS * 3u];
    CHECK(cemu_s6b33bx_render_rgb(st, 1, 1, rgb, sizeof rgb) == 0);
    CHECK(rgb[0] == 255 && rgb[1] == 0 && rgb[2] == 0);
    CHECK(rgb[3] == 0 && rgb[4] == 255 && rgb[5] == 0);
    CHECK(rgb[6] == 0 && rgb[7] == 0 && rgb[8] == 255);

    st->write_index = S6B33BX_GRAM_PIXELS - 1u;
    cemu_s6b33bx_write_pixel(S.lcd_periph, &S, 0xFFFF);
    CHECK(st->gram[S6B33BX_GRAM_PIXELS - 1u] == 0xFFFF);
    CHECK(st->write_index == 0);
    CHECK(st->common.data_seq == 4);
    cemu_soc_free(&S);
}

static void test_measured_inner_window_addressing(void) {
    setup();
    s6b33bx_state_t *st = lcd();
    memset(st->gram, 0xA5, sizeof st->gram);
    memset(st->presented_gram, 0x5A, sizeof st->presented_gram);

    set_main_selected(0);
    set_data_mode(0);
    configure_ssc(8, 1);
    set_main_selected(1);
    send_ssc(0x43);
    send_ssc(0x01);
    send_ssc(0x80);
    send_ssc(0x42);
    send_ssc(0x01);
    send_ssc(0x80);
    set_main_selected(0);
    CHECK(st->window_start_x == 1 && st->window_end_x == 128);
    CHECK(st->window_start_y == 1 && st->window_end_y == 128);
    CHECK(st->pending_window_command == 0);
    CHECK(st->write_index == 0);

    cemu_s6b33bx_write_pixel(S.lcd_periph, &S, 0);
    CHECK(st->presented_gram[1u * S6B33BX_WIDTH + 1u] == 0x5A5A);
    CHECK(st->gram[1u * S6B33BX_WIDTH + 1u] == 0);
    for (unsigned i = 1; i < 128u * 128u; i++)
        cemu_s6b33bx_write_pixel(S.lcd_periph, &S, (uint16_t)i);

    CHECK(st->write_index == 0);
    CHECK(st->gram[1u * S6B33BX_WIDTH + 1u] == 0);
    CHECK(st->gram[1u * S6B33BX_WIDTH + 128u] == 127);
    CHECK(st->gram[2u * S6B33BX_WIDTH + 1u] == 128);
    CHECK(st->gram[128u * S6B33BX_WIDTH + 128u] == 0x3FFF);
    CHECK(!memcmp(st->gram, st->presented_gram, sizeof st->gram));
    for (unsigned i = 0; i < S6B33BX_WIDTH; i++) {
        CHECK(st->gram[i] == 0xA5A5);
        CHECK(st->gram[129u * S6B33BX_WIDTH + i] == 0xA5A5);
        CHECK(st->gram[i * S6B33BX_WIDTH] == 0xA5A5);
        CHECK(st->gram[i * S6B33BX_WIDTH + 129u] == 0xA5A5);
    }

    set_main_selected(1);
    send_ssc(0x43);
    send_ssc(0x7F);
    set_main_selected(0);
    CHECK(st->pending_window_command == 0);
    CHECK(st->window_start_x == 1 && st->window_end_x == 128);
    cemu_soc_free(&S);
}

static void test_controller_axes_map_to_panel_orientation(void) {
    setup();
    s6b33bx_state_t *st = lcd();
    memset(st->gram, 0xA5, sizeof st->gram);

    cemu_s6b33bx_write_command(S.lcd_periph, &S, 0x42);
    cemu_s6b33bx_write_command(S.lcd_periph, &S, 2);
    cemu_s6b33bx_write_command(S.lcd_periph, &S, 3);
    cemu_s6b33bx_write_command(S.lcd_periph, &S, 0x43);
    cemu_s6b33bx_write_command(S.lcd_periph, &S, 5);
    cemu_s6b33bx_write_command(S.lcd_periph, &S, 7);

    CHECK(st->window_start_x == 5 && st->window_end_x == 7);
    CHECK(st->window_start_y == 2 && st->window_end_y == 3);
    for (unsigned i = 0; i < 6; i++)
        cemu_s6b33bx_write_pixel(S.lcd_periph, &S, (uint16_t)i);

    CHECK(st->gram[2u * S6B33BX_WIDTH + 5u] == 0);
    CHECK(st->gram[2u * S6B33BX_WIDTH + 6u] == 1);
    CHECK(st->gram[2u * S6B33BX_WIDTH + 7u] == 2);
    CHECK(st->gram[3u * S6B33BX_WIDTH + 5u] == 3);
    CHECK(st->gram[3u * S6B33BX_WIDTH + 6u] == 4);
    CHECK(st->gram[3u * S6B33BX_WIDTH + 7u] == 5);
    CHECK(st->write_index == 0);
    cemu_soc_free(&S);
}

static void test_full_transport_frame_and_digest_restore(void) {
    setup();
    cpu_t cpu;
    cemu_cpu_init(&cpu, &S.bus);
    cemu_soc_attach_cpu(&S, &cpu);
    cemu_cpu_reset(&cpu);
    debugger_t debugger;
    debugger_init(&debugger, &cpu, &S, 1);
    s6b33bx_state_t *st = lcd();

    set_main_selected(0);
    set_data_mode(1);
    configure_ssc(16, 1);
    set_main_selected(1);
    for (unsigned i = 0; i < S6B33BX_GRAM_PIXELS; i++)
        send_ssc((uint16_t)i);
    CHECK(st->write_index == 0);
    CHECK(st->common.data_seq == S6B33BX_GRAM_PIXELS);
    CHECK(st->common.transaction_data_bytes ==
          S6B33BX_GRAM_PIXELS * 2u);
    set_main_selected(0);
    CHECK(st->common.frame_seq == 1);
    CHECK(st->gram[0] == 0);
    CHECK(st->gram[S6B33BX_GRAM_PIXELS - 1u] ==
          (uint16_t)(S6B33BX_GRAM_PIXELS - 1u));
    CHECK(st->common.unsupported_transfers == 0);

    uint64_t before = state_digest(&cpu, &S);
    debugger_checkpoint(&debugger);
    st->gram[17] ^= 0xFFFFu;
    CHECK(state_digest(&cpu, &S) != before);
    CHECK(debugger_restore(&debugger) == 1);
    CHECK(state_digest(&cpu, &S) == before);

    debugger_detach(&debugger);
    debugger_free(&debugger);
    cemu_soc_free(&S);
}

typedef struct { const char *name; void (*fn)(void); } entry_t;
static const entry_t TESTS[] = {
    {"registration_and_geometry", test_registration_and_geometry},
    {"main_select_rs_and_sub_isolation",
     test_main_select_rs_and_sub_isolation},
    {"measured_commands_and_transfer_validation",
     test_measured_commands_and_transfer_validation},
    {"rgb565_renderer_calibration_and_wrap",
     test_rgb565_renderer_calibration_and_wrap},
    {"measured_inner_window_addressing",
     test_measured_inner_window_addressing},
    {"controller_axes_map_to_panel_orientation",
     test_controller_axes_map_to_panel_orientation},
    {"full_transport_frame_and_digest_restore",
     test_full_transport_frame_and_digest_restore},
};

int main(void) {
    int n = (int)(sizeof TESTS / sizeof TESTS[0]);
    for (int i = 0; i < n; i++) {
        g_fail = 0;
        TESTS[i].fn();
        g_total_run++;
        g_total_fail += g_fail;
        if (g_fail) printf("[FAIL] %s (%d)\n", TESTS[i].name, g_fail);
    }
    free(flash);
    printf("\n%d tests, %d failed\n", g_total_run, g_total_fail);
    return g_total_fail ? 1 : 0;
}
