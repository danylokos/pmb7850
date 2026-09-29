#include "common.h"

/* SW15 table at file 0x84FF0E; values match A60/A62, in a different order. */
static const keypad_button_config_t A65_KEYPAD[] = {
    {"0", 0x020B, 0x30}, {"1", 0x0107, 0x31},
    {"2", 0x010B, 0x32}, {"3", 0x010D, 0x33},
    {"4", 0x0087, 0x34}, {"5", 0x008B, 0x35},
    {"6", 0x008D, 0x36}, {"7", 0x0047, 0x37},
    {"8", 0x004B, 0x38}, {"9", 0x004D, 0x39},
    {"star", 0x0207, 0x2A}, {"hash", 0x020D, 0x23},
    {"up", 0x008E, 0x3B}, {"down", 0x004E, 0x3C},
    {"right", 0x020E, 0x3E}, {"left", 0x010E, 0x3D},
    {"power", 0x000D, 0x0C}, {"send", 0x0017, 0x0B},
    {"soft-right", 0x001B, 0x04}, {"soft-left", 0x001E, 0x01},
};

const device_config_t cemu_device_config_a65 = {
    .name = "a65",
    .xbus_unknown1_id = 0x1203,
    .irq55_sources = DEVICE_IRQ55_SOURCE_KEYPAD_ACTIVITY,
    DEVICE_AM29LV128_FLASH,
    DEVICE_2M_PSRAM,
    DEVICE_COMMON_STRAPS,
    .serial_link = {
        .available = 1,
        .port = 7,
        .bit = 3,
        .idle_level = 1,
    },
    .ports = {
        .inputs = {{7, 0x14A0, 0x0420},
                   {8, 0x4000, 0x4000},
                   {3, 0x0400, 0x0000}},
        .count = 3,
    },
    /* P7.5/P7.7 high/low selects LCD ID 0x22. SW15 low/low instead
     * selects ID 0x2e and its PCF8833-compatible RGB444 driver. */
    DEVICE_HM17_COLOR_LCD,
    DEVICE_COLOR_KEYPAD(0x0000, A65_KEYPAD),
    DEVICE_CEMU_BATTERY(DEVICE_BATTERY_RESULT0),
    .audio = {
        .clock_hz = 26000000,
        .stream_profile = DEVICE_AUDIO_STREAM_XBUS_UNKNOWN1_V1,
    },
};
