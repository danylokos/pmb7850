#include "common.h"

const device_config_t cemu_device_config_s55 = {
    .name = "s55",
    .xbus_unknown1_id = 0x1202,
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
    .lcd = {
        .model = "hm17cm256",
        .select_addr = 0xFFC4, .select_bit = 11, .select_active_low = 1,
        .dc_addr = 0xFFD0, .dc_bit = 6,
        .gpio_clock_addr = 0xFFD0, .gpio_clock_bit = 5,
        .gpio_data_addr = 0xFFD0, .gpio_data_bit = 7,
        .reset_capcom_channel = -1,
        .panel_width = 101, .panel_height = 80,
        .panel_origin_x = 13, .panel_mirror_x = 1,
    },
    DEVICE_COLOR_KEYPAD(0x0000, cemu_device_m55_s55_keypad),
    .ports = {
        .inputs = {{7, 0x3C00, 0x2800}, {6, 0x0020, 0x0020}},
        .count = 2,
    },
    DEVICE_CEMU_BATTERY(DEVICE_BATTERY_RESULT1),
};
