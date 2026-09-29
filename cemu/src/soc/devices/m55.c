#include "common.h"

const device_config_t cemu_device_config_m55 = {
    .name = "m55",
    .xbus_unknown1_id = 0x1203,
    .irq55_sources = DEVICE_IRQ55_SOURCES_DEFAULT,
    DEVICE_AM29LV128_FLASH,
    DEVICE_2M_PSRAM,
    DEVICE_COMMON_STRAPS,
    .serial_link = {
        .available = 1,
        .port = 7,
        .bit = 3,
        .idle_level = 1,
    },
    .lcd = {
        .model = "hm17cm4096",
        .select_addr = 0xFFC4, .select_bit = 11, .select_active_low = 1,
        .dc_addr = 0xFFD0, .dc_bit = 6,
        .gpio_clock_addr = 0xFFD0, .gpio_clock_bit = 5,
        .gpio_data_addr = 0xFFD0, .gpio_data_bit = 7,
        .reset_capcom_channel = 0, .reset_capcom_idle_level = 1,
        .panel_width = 101, .panel_height = 80,
        .panel_origin_x = 24, .panel_mirror_x = 1,
    },
    DEVICE_COLOR_KEYPAD(0x0000, cemu_device_m55_s55_keypad),
    .ports = {
        .inputs = {{7, 0x3000, 0x2000}, {6, 0x0020, 0x0000}},
        .count = 2,
    },
    DEVICE_CEMU_BATTERY(DEVICE_BATTERY_RESULT0),
    .audio = {
        .capcom_ringer_present = 1,
        .capcom_ringer_channel = 2,
        .clock_hz = 26000000,
        .stream_profile = DEVICE_AUDIO_STREAM_XBUS_UNKNOWN1_V1,
    },
};
