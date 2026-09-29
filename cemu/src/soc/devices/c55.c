#include "common.h"

const device_config_t cemu_device_config_c55 = {
    .name = "c55",
    .xbus_unknown1_id = 0x1202,
    .irq55_sources = DEVICE_IRQ55_SOURCES_DEFAULT,
    DEVICE_COMMON_M58_FLASH,
    DEVICE_512K_SRAM,
    DEVICE_COMMON_STRAPS,
    .serial_link = {
        .available = 1,
        .port = 7,
        .bit = 3,
        .idle_level = 1,
    },
    DEVICE_COMMON_LCD,
    DEVICE_COMMON_KEYPAD,
    .baseband = {
        .software_version = 24,
        .response_carrier_addr = 0x2A6CC,
        .response_flags_addr = 0x2A6CE,
    },
    .gsm_legacy_adapter = {
        .registration_adapter_pc = 0xBB4382,
        .registration_home_plmn_addr = 0x157996,
        .registration_network_state_addr = 0x157972,
        .registration_candidate_ptr_addr = 0x100378,
        .registration_candidate_seg_addr = 0x10037A,
        .registration_signal_setter_pc = 0xBC6CAA,
        .registration_signal_level = 0x20,
        /* These firmware-owned gates are stable by 200M instructions. */
        .registration_min_icount = 200000000,
    },
    DEVICE_CEMU_BATTERY(DEVICE_BATTERY_RESULT1),
    .audio = {
        .capcom_ringer_present = 1,
        .capcom_ringer_channel = 2,
        .clock_hz = 26000000,
        .stream_profile = DEVICE_AUDIO_STREAM_XBUS_UNKNOWN1_V1,
    },
};
