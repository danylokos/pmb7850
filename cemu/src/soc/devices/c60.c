#include "common.h"

const device_config_t cemu_device_config_c60 = {
    .name = "c60",
    .xbus_unknown1_id = 0x1203,
    .irq55_sources = DEVICE_IRQ55_SOURCE_KEYPAD_ACTIVITY,
    .flash = {
        .chips = {{.name = "flash", .model = "am29lv128mh",
                   .chip_size = DEVICE_MIB(16)}},
        .nchips = 1,
        .windows = {
            DEVICE_FLASH_WINDOW(0x000000, DEVICE_MIB(16), 0, 0,
                                DEVICE_MIB(16), 1),
        },
        .nwindows = 1,
        .eeprom_overlay_identity = {
            EEPROM_OVERLAY_IDENTITY_AM29_SECSI, 0
        },
    },
    DEVICE_2M_PSRAM,
    DEVICE_COMMON_STRAPS,
    .serial_link = {
        .available = 1,
        .port = 7,
        .bit = 3,
        .idle_level = 1,
    },
    .ports = {
        .inputs = {{7, 0x1400, 0x0400},
                   {8, 0x4000, 0x0000},
                   {3, 0x0400, 0x0400}},
        .count = 3,
    },
    DEVICE_HM17_COLOR_LCD,
    DEVICE_COLOR_KEYPAD(0x0000, cemu_device_a60_keypad),
    DEVICE_CEMU_BATTERY(DEVICE_BATTERY_RESULT0),
    .audio = {
        .capcom_ringer_present = 1,
        .capcom_ringer_channel = 2,
        .clock_hz = 26000000,
        .stream_profile = DEVICE_AUDIO_STREAM_XBUS_UNKNOWN1_V1,
    },
};
