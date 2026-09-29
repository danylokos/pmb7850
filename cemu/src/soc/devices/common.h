#ifndef CEMU_DEVICE_CONFIG_COMMON_H
#define CEMU_DEVICE_CONFIG_COMMON_H

#include "devices.h"

#define DEVICE_COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#define DEVICE_MIB(n) ((uint32_t)(n) * 1024u * 1024u)

#define DEVICE_FLASH_WINDOW(cpu, size, index, chip, period, commands) \
    { .cpu_base = (cpu), .cpu_size = (size), .chip_index = (index), \
      .chip_base = (chip), .mirror_period = (period), \
      .command_visible = (commands) }

#define DEVICE_COMMON_M58_FLASH \
    .flash = { \
        .chips = {{.name = "flash", .model = "m58lw064d", \
                   .chip_size = DEVICE_MIB(8)}}, \
        .nchips = 1, \
        .windows = { \
            DEVICE_FLASH_WINDOW(0x000000, DEVICE_MIB(8), 0, 0, \
                                DEVICE_MIB(8), 0), \
            DEVICE_FLASH_WINDOW(0x800000, DEVICE_MIB(8), 0, 0, \
                                DEVICE_MIB(8), 1), \
        }, \
        .nwindows = 2, \
        .eeprom_overlay_identity = { \
            EEPROM_OVERLAY_IDENTITY_FACTORY_UID, 0 \
        }, \
    }

#define DEVICE_DUAL_W30_M58_FLASH \
    .flash = { \
        .chips = { \
            {.name = "flash-primary", .model = "w30-64mbit-top", \
             .chip_size = DEVICE_MIB(8)}, \
            {.name = "flash-secondary", .model = "m58lw064d", \
             .chip_size = DEVICE_MIB(4)}, \
        }, \
        .nchips = 2, \
        .windows = { \
            DEVICE_FLASH_WINDOW(0x000000, DEVICE_MIB(4), 0, 0, \
                                DEVICE_MIB(4), 0), \
            DEVICE_FLASH_WINDOW(0x400000, DEVICE_MIB(4), 1, 0, \
                                DEVICE_MIB(4), 1), \
            DEVICE_FLASH_WINDOW(0x800000, DEVICE_MIB(8), 0, 0, \
                                DEVICE_MIB(8), 1), \
        }, \
        .nwindows = 3, \
        .eeprom_overlay_identity = { \
            EEPROM_OVERLAY_IDENTITY_FACTORY_UID, 0 \
        }, \
    }

#define DEVICE_AM29LV128_FLASH \
    .flash = { \
        .chips = {{.name = "flash", .model = "am29lv128mh", \
                   .chip_size = DEVICE_MIB(16), \
                   .protected_start = 0x800000, \
                   .protected_size = 0x040000, \
                   .secsi_factory_locked = 1}}, \
        .nchips = 1, \
        .windows = { \
            DEVICE_FLASH_WINDOW(0x000000, DEVICE_MIB(16), 0, 0, \
                                DEVICE_MIB(16), 1), \
        }, \
        .nwindows = 1, \
        .eeprom_overlay_identity = { \
            EEPROM_OVERLAY_IDENTITY_AM29_SECSI, 0 \
        }, \
    }

#define DEVICE_COMMON_STRAPS \
    .straps = { .entries = {{0xF13C, 0x0002}}, .count = 1 }

#define DEVICE_512K_SRAM \
    .external_ram = { \
        .devices = {{.model = "sram-256kx16", \
                     .chip_size = 512u * 1024u, .addrsel_index = 2}}, \
        .count = 1, \
    }

#define DEVICE_1M_SRAM \
    .external_ram = { \
        .devices = {{.model = "sram-512kx16", \
                     .chip_size = DEVICE_MIB(1), .addrsel_index = 2}}, \
        .count = 1, \
    }

#define DEVICE_2M_PSRAM \
    .external_ram = { \
        .devices = {{.model = "psram-1024kx16", \
                     .chip_size = DEVICE_MIB(2), .addrsel_index = 1}}, \
        .count = 1, \
    }

enum {
    DEVICE_CEMU_BATTERY_CURVE_COUNT = 25,
};

extern const battery_curve_point_t cemu_device_battery_curve[
    DEVICE_CEMU_BATTERY_CURVE_COUNT];

#define DEVICE_CEMU_BATTERY(RESULT_WORD) \
    .battery = { \
        .available = 1, \
        .curve = cemu_device_battery_curve, \
        .n_curve = DEVICE_COUNT_OF(cemu_device_battery_curve), \
        .adc_raw_high = -1628, \
        .adc_mv_high = 4178, \
        .adc_raw_low = 4499, \
        .adc_mv_low = 3177, \
        .battery_channel = 1, \
        .reference_channel = 3, \
        .adc_result_word = (RESULT_WORD), \
    }

#define DEVICE_COMMON_LCD \
    .lcd = { .model = "pcf8813", .select_addr = 0xFFC4, .select_bit = 11, \
             .select_active_low = 1, .dc_addr = 0xFFD0, .dc_bit = 6, \
             .gpio_clock_addr = 0xFFD0, .gpio_clock_bit = 5, \
             .gpio_data_addr = 0xFFD0, \
             .gpio_data_direction_addr = 0xFFD2, .gpio_data_bit = 7, \
             .reset_capcom_channel = -1, \
             .panel_width = 101, .panel_height = 64, .panel_origin_x = 1, \
             .panel_mirror_x = 1, .panel_mirror_y = 1 }

#define DEVICE_HM17_COLOR_LCD \
    .lcd = { .model = "hm17cm4096", \
             .select_addr = 0xFFC4, .select_bit = 11, \
             .select_active_low = 1, .dc_addr = 0xFFD4, .dc_bit = 13, \
             .gpio_clock_addr = 0xFFD0, .gpio_clock_bit = 5, \
             .gpio_data_addr = 0xFFD0, .gpio_data_bit = 7, \
             .reset_capcom_channel = -1, \
             .panel_width = 101, .panel_height = 80, \
             .panel_origin_x = 24, .panel_mirror_x = 1 }

enum {
    DEVICE_C55_KEYPAD_COUNT = 18,
    DEVICE_A60_KEYPAD_COUNT = 20,
    DEVICE_M55_S55_KEYPAD_COUNT = 20,
};

extern const keypad_button_config_t
    cemu_device_c55_keypad[DEVICE_C55_KEYPAD_COUNT];
extern const keypad_button_config_t
    cemu_device_a60_keypad[DEVICE_A60_KEYPAD_COUNT];
extern const keypad_button_config_t
    cemu_device_m55_s55_keypad[DEVICE_M55_S55_KEYPAD_COUNT];

#define DEVICE_COMMON_KEYPAD \
    .keypad = { .scan_command = 0x003D, \
                .idle_result = 0x000F, .matrix_idle_result = 0x000F, \
                .buttons = cemu_device_c55_keypad, \
                .nbuttons = DEVICE_C55_KEYPAD_COUNT, \
                .matrix_commands = {0xFC,0xFD,0xF9,0xF5,0xED,0xDD}, \
                .n_matrix_commands = 6 }

#define DEVICE_COLOR_KEYPAD(scan, keymap) \
    .keypad = { .scan_command = (scan), \
                .idle_result = 0xFF0F, .matrix_idle_result = 0x000F, \
                .release_activity_on_last_key_up = 1, \
                .buttons = (keymap), \
                .nbuttons = DEVICE_COUNT_OF(keymap), \
                .matrix_select_addr = 0xFFC6, \
                .matrix_select_mask = 0x003D }

#endif
