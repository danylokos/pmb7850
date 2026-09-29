#include "common.h"

const device_config_t cemu_device_config_a55 = {
    .name = "a55",
    .xbus_unknown1_id = 0x1202,
    .irq55_sources = DEVICE_IRQ55_SOURCES_DEFAULT,
    DEVICE_COMMON_M58_FLASH,
    DEVICE_512K_SRAM,
    DEVICE_COMMON_STRAPS,
    /* LCD wiring copied from C55 and intentionally remains unverified. */
    DEVICE_COMMON_LCD,
    DEVICE_COMMON_KEYPAD,
    .ports = { .inputs = {{7, 0x3400, 0x1400}}, .count = 1 },
    DEVICE_CEMU_BATTERY(DEVICE_BATTERY_RESULT1),
    .audio = {
        .capcom_ringer_present = 1,
        .capcom_ringer_channel = 2,
        .clock_hz = 26000000,
        .stream_profile = DEVICE_AUDIO_STREAM_XBUS_UNKNOWN1_V1,
    },
};
