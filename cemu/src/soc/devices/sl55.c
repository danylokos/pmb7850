#include "common.h"

/* SW09 table at CPU 0x5B36DC; SW07 at file 0x1AFF0C is byte-identical.
 * Three additional entries map to unidentified sidekey/slider controls. */
static const keypad_button_config_t SL55_KEYPAD[] = {
    {"0", 0x004D, 0x30}, {"1", 0x001E, 0x31},
    {"2", 0x004E, 0x32}, {"3", 0x008E, 0x33},
    {"4", 0x0017, 0x34}, {"5", 0x0047, 0x35},
    {"6", 0x0087, 0x36}, {"7", 0x001B, 0x37},
    {"8", 0x004B, 0x38}, {"9", 0x008B, 0x39},
    {"star", 0x001D, 0x2A}, {"hash", 0x008D, 0x23},
    {"up", 0x010E, 0x3B}, {"down", 0x010D, 0x3C},
    {"left", 0x0107, 0x3D}, {"right", 0x010B, 0x3E},
    {"soft-left", 0x0007, 0x01}, {"soft-right", 0x000B, 0x04},
    {"send", 0x000E, 0x0B}, {"power", 0x000D, 0x0C},
};

const device_config_t cemu_device_config_sl55 = {
    .name = "sl55",
    .xbus_unknown1_id = 0x1203,
    .irq55_sources = DEVICE_IRQ55_SOURCES_DEFAULT,
    DEVICE_DUAL_W30_M58_FLASH,
    DEVICE_1M_SRAM,
    DEVICE_COMMON_STRAPS,
    .serial_link = {
        .available = 1,
        .port = 7,
        .bit = 3,
        .idle_level = 1,
    },
    /* Measured 8-bit controls and 12-bit RGB444 data use opposite RS
     * polarity from M55. Exact controller silicon remains unverified. */
    .lcd = {
        .model = "pcf8833-4wire",
        .select_addr = 0xFFC4, .select_bit = 11, .select_active_low = 1,
        .dc_addr = 0xFFD0, .dc_bit = 6,
        .gpio_clock_addr = 0xFFD0, .gpio_clock_bit = 5,
        .gpio_data_addr = 0xFFD0, .gpio_data_bit = 7,
        .reset_capcom_channel = 0, .reset_capcom_idle_level = 1,
        .panel_width = 101, .panel_height = 80,
        .panel_origin_x = 1, .panel_mirror_x = 1,
    },
    .keypad = {
        .scan_command = 0x003D,
        .idle_result = 0xFF0F,
        .matrix_idle_result = 0x000F,
        .release_activity_on_last_key_up = 1,
        .buttons = SL55_KEYPAD,
        .nbuttons = DEVICE_COUNT_OF(SL55_KEYPAD),
        .matrix_commands = {0xFE,0xFB,0xF7,0xEF,0xDF},
        .n_matrix_commands = 5,
        .matrix_select_addr = 0xFFC6,
        .matrix_select_mask = 0x003D,
    },
    .ports = {
        .inputs = {{6, 0x0008, 0x0008}, {7, 0x0200, 0x0200}},
        .count = 2,
    },
    DEVICE_CEMU_BATTERY(DEVICE_BATTERY_RESULT0),
};
