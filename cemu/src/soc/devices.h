/* Per-device configuration declarations. docs/ANALYSIS.md owns the evidence. */
#ifndef CEMU_DEVICES_H
#define CEMU_DEVICES_H

#include <stddef.h>
#include <stdint.h>

#define MAX_FLASH_CHIPS 2
#define MAX_FLASH_WINDOWS 4
#define MAX_EXTERNAL_RAM_DEVICES 2
#define MAX_STRAPS 2
#define MAX_PORT_INPUTS 4
#define MAX_KEYPAD_SCAN_COMMANDS 8
#define MAX_TWI_REGISTERS 256
typedef struct {
    uint32_t cpu_base;
    uint32_t cpu_size;
    int chip_index;
    uint32_t chip_base;
    uint32_t mirror_period;
    int command_visible;
} flash_window_config_t;

typedef struct {
    const char *name;
    const char *model;
    uint32_t chip_size;
    uint32_t protected_start;
    uint32_t protected_size;
    int secsi_factory_locked;
} flash_chip_config_t;

typedef enum {
    EEPROM_OVERLAY_IDENTITY_NONE = 0,
    EEPROM_OVERLAY_IDENTITY_FACTORY_UID,
    EEPROM_OVERLAY_IDENTITY_AM29_SECSI,
} eeprom_overlay_identity_kind_t;

typedef struct {
    eeprom_overlay_identity_kind_t kind;
    int chip_index;
} eeprom_overlay_identity_config_t;

typedef struct {
    flash_chip_config_t chips[MAX_FLASH_CHIPS];
    int nchips;
    flash_window_config_t windows[MAX_FLASH_WINDOWS];
    int nwindows;
    eeprom_overlay_identity_config_t eeprom_overlay_identity;
} device_flash_config_t;

typedef struct {
    size_t file_offsets[MAX_FLASH_CHIPS];
    int count;
} device_flash_image_mapping_t;

typedef struct {
    const char *model;
    uint32_t chip_size;
    int addrsel_index;
} external_ram_config_t;

typedef struct {
    external_ram_config_t devices[MAX_EXTERNAL_RAM_DEVICES];
    int count;
} device_external_ram_config_t;

typedef struct {
    struct { uint32_t addr; uint16_t val; } entries[MAX_STRAPS];
    int count;
} device_strap_config_t;

typedef struct {
    struct { uint8_t port; uint16_t mask; uint16_t value; } inputs[MAX_PORT_INPUTS];
    int count;
} device_port_config_t;

/* Firmware-visible ASC0 receive-idle input. Unsupported devices leave
 * `available` zero; the PTY byte bridge holds this input at the documented
 * asynchronous idle level without attempting to model individual bits. */
typedef struct {
    int available;
    uint8_t port;
    uint8_t bit;
    uint8_t idle_level;
} device_serial_link_config_t;

typedef struct {
    int available;
    const char *model;
    uint8_t address;
    uint8_t port;
    uint8_t scl_bit;
    uint8_t sda_bit;
    uint16_t register_count;
    uint8_t registers[MAX_TWI_REGISTERS];
} device_twi_config_t;

typedef struct {
    const char *model;
    uint32_t select_addr;
    int select_bit;
    int select_active_low;
    uint32_t dc_addr;
    int dc_bit;
    uint32_t gpio_clock_addr;
    int gpio_clock_bit;
    uint32_t gpio_data_addr;
    uint32_t gpio_data_direction_addr;
    int gpio_data_bit;
    int reset_capcom_channel;
    int reset_capcom_idle_level;
    unsigned panel_width;
    unsigned panel_height;
    unsigned panel_origin_x;
    unsigned panel_origin_y;
    int panel_mirror_x;
    int panel_mirror_y;
} device_lcd_config_t;

typedef struct {
    const char *name;
    uint16_t raw_code;
    uint8_t logical_code;
} keypad_button_config_t;

typedef struct {
    uint16_t scan_command;
    uint16_t idle_result;
    uint16_t matrix_idle_result;
    int release_activity_on_last_key_up;
    const keypad_button_config_t *buttons;
    size_t nbuttons;
    uint8_t matrix_commands[MAX_KEYPAD_SCAN_COMMANDS];
    size_t n_matrix_commands;
    uint32_t matrix_select_addr;
    uint16_t matrix_select_mask;
} device_keypad_config_t;

enum {
    DEVICE_IRQ55_SOURCE_CC23 = 1u << 0,
    DEVICE_IRQ55_SOURCE_KEYPAD_ACTIVITY = 1u << 1,
};

#define DEVICE_IRQ55_SOURCES_DEFAULT \
    (DEVICE_IRQ55_SOURCE_CC23 | DEVICE_IRQ55_SOURCE_KEYPAD_ACTIVITY)

/* Firmware-evidenced baseband transport geometry.  This profile describes
 * only locations owned by the baseband producer; it must not name firmware
 * functions, UI state, or guest-owned service/signal variables. */
typedef struct {
    int software_version;
    uint32_t response_carrier_addr;
    uint32_t response_flags_addr;
} device_baseband_profile_t;

/* Temporary compatibility adapter for the C55 SW24 far-call experiment.
 * Keep this separate from the baseband profile so it cannot become part of
 * the shared guest-facing transport contract. */
typedef struct {
    uint32_t registration_adapter_pc;
    uint32_t registration_home_plmn_addr;
    uint32_t registration_network_state_addr;
    uint32_t registration_candidate_ptr_addr;
    uint32_t registration_candidate_seg_addr;
    uint32_t registration_signal_setter_pc;
    uint16_t registration_signal_level;
    uint64_t registration_min_icount;
} device_gsm_legacy_adapter_config_t;

typedef struct {
    uint8_t level;
    uint16_t millivolts;
} battery_curve_point_t;

enum {
    DEVICE_BATTERY_RESULT0 = 0,
    DEVICE_BATTERY_RESULT1 = 1,
};

/* Device-selected calibration for the host-facing battery control. The shared
 * analog model consumes this data; unsupported devices leave `available`
 * zero. Generated EEPROM overlays carry matching firmware calibration. */
typedef struct {
    int available;
    const battery_curve_point_t *curve;
    size_t n_curve;
    int16_t adc_raw_high;
    uint16_t adc_mv_high;
    int16_t adc_raw_low;
    uint16_t adc_mv_low;
    uint8_t battery_channel;
    uint8_t reference_channel;
    uint8_t adc_result_word;
} device_battery_config_t;

typedef enum {
    DEVICE_AUDIO_STREAM_NONE = 0,
    DEVICE_AUDIO_STREAM_XBUS_UNKNOWN1_V1,
} device_audio_stream_profile_t;

/* Board-routed digital audio capabilities. This models only the qualified
 * SoC-facing routes; downstream analog behavior remains outside CEMU. The
 * XBUS profile owns its hardware-register ABI and packet policy. */
typedef struct {
    int capcom_ringer_present;
    int capcom_ringer_channel;
    uint32_t clock_hz;
    device_audio_stream_profile_t stream_profile;
} device_audio_config_t;

typedef struct {
    const char *name;
    uint16_t xbus_unknown1_id;
    uint8_t irq55_sources;
    device_flash_config_t flash;
    device_flash_image_mapping_t flash_image;
    device_external_ram_config_t external_ram;
    device_strap_config_t straps;
    device_port_config_t ports;
    device_serial_link_config_t serial_link;
    device_twi_config_t twi;
    device_lcd_config_t lcd;
    device_keypad_config_t keypad;
    device_baseband_profile_t baseband;
    device_gsm_legacy_adapter_config_t gsm_legacy_adapter;
    device_battery_config_t battery;
    device_audio_config_t audio;
} device_config_t;

const device_config_t *cemu_device_by_name(const char *name);
/* Validate a host-resolved physical description against private board wiring
 * without reading or interpreting source bytes. */
int cemu_device_config_from_prepared(const char *name,
                                     const char *const chip_models[],
                                     const size_t chip_offsets[],
                                     const size_t chip_sizes[],
                                     size_t chip_count, size_t image_size,
                                     device_config_t *cfg,
                                     char *error, size_t error_size);
int cemu_device_flash_config_validate(const device_flash_config_t *cfg,
                                 char *error, size_t error_size);
int cemu_device_flash_image_validate(const device_config_t *cfg,
                                size_t image_size,
                                char *error, size_t error_size);
int cemu_device_external_ram_config_validate(const device_external_ram_config_t *cfg,
                                        char *error, size_t error_size);

#endif
