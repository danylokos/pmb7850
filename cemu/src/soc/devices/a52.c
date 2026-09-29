#include "common.h"

const device_config_t cemu_device_config_a52 = {
    .name = "a52",
    .xbus_unknown1_id = 0x1202,
    .irq55_sources = DEVICE_IRQ55_SOURCES_DEFAULT,
    .flash = {
        .chips = {{.name = "flash", .model = "m58lw064d",
                   .chip_size = DEVICE_MIB(4)}},
        .nchips = 1,
        .windows = {
            DEVICE_FLASH_WINDOW(0x000000, DEVICE_MIB(8), 0, 0,
                                DEVICE_MIB(4), 0),
            DEVICE_FLASH_WINDOW(0x800000, DEVICE_MIB(8), 0, 0,
                                DEVICE_MIB(4), 1),
        },
        .nwindows = 2,
        .eeprom_overlay_identity = {
            EEPROM_OVERLAY_IDENTITY_FACTORY_UID, 0
        },
    },
    DEVICE_512K_SRAM,
    DEVICE_COMMON_STRAPS,
    /* Observed P3.11/P7.6/P7.5/P7.7 wiring; exact LCD die unverified. */
    DEVICE_COMMON_LCD,
    DEVICE_COMMON_KEYPAD,
    DEVICE_CEMU_BATTERY(DEVICE_BATTERY_RESULT1),
    .audio = {
        .capcom_ringer_present = 1,
        .capcom_ringer_channel = 2,
        .clock_hz = 26000000,
        .stream_profile = DEVICE_AUDIO_STREAM_XBUS_UNKNOWN1_V1,
    },
};
