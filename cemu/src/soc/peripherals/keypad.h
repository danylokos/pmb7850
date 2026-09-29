/* Evidence-bounded x55 startup-key scanner and per-device matrix. */
#ifndef CEMU_PERIPH_KEYPAD_H
#define CEMU_PERIPH_KEYPAD_H

#include <stddef.h>
#include <stdint.h>
#include "peripheral.h"
#include "devices.h"

#define KEYPAD_SCAN_RESULT  0xEF1Au
#define KEYPAD_SCAN_COMMAND 0xEF1Cu

typedef keypad_button_config_t keypad_button_t;

typedef struct {
    uint64_t scans;
    uint64_t handled_scans;
    uint64_t results;
    uint64_t startup_releases;
    uint32_t pressed;
    uint32_t sampled_pressed;
    uint16_t last_command;
    uint16_t last_result;
    uint8_t startup_power_pending;
    uint8_t startup_result_tagged;
    uint8_t startup_result_read_mask;
    uint8_t matrix_scan_phase;
    uint8_t release_activity_pending;
} keypad_mutable_state_t;

typedef struct {
    uint16_t scan_command;
    uint16_t idle_result;
    uint16_t matrix_idle_result;
    uint8_t release_activity_on_last_key_up;
    uint8_t release_activity_pending;
    const keypad_button_t *buttons;
    size_t nbuttons;
    uint8_t matrix_commands[MAX_KEYPAD_SCAN_COMMANDS];
    size_t n_matrix_commands;
    uint32_t matrix_select_addr;
    uint16_t matrix_select_mask;
    uint32_t activity_ic_addr;
    uint32_t sfr_words[1];
    size_t n_sfr_words;
    uint64_t scans;
    uint64_t handled_scans;
    uint64_t results;
    uint64_t startup_releases;
    uint16_t last_command;
    uint16_t last_result;
    uint32_t pressed;
    uint32_t sampled_pressed;
    uint8_t startup_power_pending;
    uint8_t startup_result_tagged;
    uint8_t startup_result_read_mask;
    uint8_t matrix_scan_phase;
    addr_range_t ranges[1];
    reg_name_t reg_names[2];
} keypad_state_t;

void cemu_keypad_periph_init(peripheral_t *p, keypad_state_t *st,
                        const device_keypad_config_t *cfg,
                        uint32_t activity_ic_addr);
const keypad_button_t *cemu_keypad_buttons(const keypad_state_t *st, size_t *count);
const keypad_button_t *cemu_keypad_button_by_name(const keypad_state_t *st,
                                              const char *name);
int cemu_keypad_set_button(keypad_state_t *st, soc_t *s, const char *name,
                      int pressed);
uint32_t cemu_keypad_sampled_buttons(const keypad_state_t *st);
uint16_t cemu_keypad_startup_power_result(const keypad_state_t *st);
void cemu_keypad_capture_mutable(const keypad_state_t *st,
                                 keypad_mutable_state_t *out);
int cemu_keypad_restore_mutable(keypad_state_t *st,
                                const keypad_mutable_state_t *saved);
void cemu_keypad_restore_legacy(keypad_state_t *st);

#endif /* CEMU_PERIPH_KEYPAD_H */
