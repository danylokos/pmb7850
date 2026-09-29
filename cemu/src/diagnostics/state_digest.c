#include <string.h>
#include "flash.h"
#include "lcd.h"
#include "sim.h"
#include "serial.h"
#include "ssc0.h"
#include "gpt1.h"
#include "gpt2.h"
#include "capcom1.h"
#include "capcom2.h"
#include "xbus_unknown1.h"
#include "battery.h"
#include "keypad.h"
#include "twi_gpio.h"
#include "gsm_stub.h"
#include "gsm_legacy_adapter.h"
#include "state_digest.h"

typedef struct {
    uint64_t value;
} digest_t;

static void add_bytes(digest_t *d, const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < size; i++) {
        d->value ^= bytes[i];
        d->value *= UINT64_C(1099511628211);
    }
}

#define ADD(d, value) do { \
    uint64_t digest_value = (uint64_t)(value); \
    add_bytes((d), &digest_value, sizeof(digest_value)); \
} while (0)

uint64_t state_digest(const cpu_t *cpu, const soc_t *soc) {
    digest_t d = { UINT64_C(1469598103934665603) };
    static const char domain[] = "cemu-state-v20";
    add_bytes(&d, domain, sizeof domain);

    ADD(&d, cpu->csp);
    ADD(&d, cpu->ip);
    ADD(&d, cpu->ext_kind);
    ADD(&d, cpu->ext_val);
    ADD(&d, cpu->ext_count);
    ADD(&d, cpu->extr);
    ADD(&d, cpu->halted);
    ADD(&d, cpu->idle);
    ADD(&d, cpu->icount);
    ADD(&d, cpu->interrupts_delivered);
    ADD(&d, cpu->traps_taken);
    ADD(&d, cpu->unimpl);
    ADD(&d, cpu->unimpl_op);
    ADD(&d, cpu->unimpl_pc);

    ADD(&d, soc->synth_mask);
    ADD(&d, soc->ticks);
    ADD(&d, soc->init_locked);
    ADD(&d, soc->rstout);
    add_bytes(&d, soc->memory.sfr, sizeof soc->memory.sfr);
    add_bytes(&d, soc->memory.present, ADDR_SPACE);
    add_bytes(&d, soc->memory.ram, ADDR_SPACE);
    ADD(&d, soc->memory.lm_size);
    add_bytes(&d, soc->memory.lm, soc->memory.lm_size);
    ADD(&d, soc->memory.n_external_ram);
    for (int i = 0; i < soc->memory.n_external_ram; i++) {
        external_ram_state_t *ram = cemu_memory_controller_external_ram_state(&soc->memory, i);
        ADD(&d, ram->chip_size);
        ADD(&d, ram->addrsel_index);
        add_bytes(&d, ram->bytes, ram->chip_size);
    }
    ADD(&d, soc->serial_tx_len);
    add_bytes(&d, soc->serial_tx, soc->serial_tx_len);
    size_t serial_rx_pending = soc->serial_rx_len - soc->serial_rx_head;
    ADD(&d, serial_rx_pending);
    if (serial_rx_pending)
        add_bytes(&d, soc->serial_rx + soc->serial_rx_head,
                  serial_rx_pending);
    const serial_state_t *serial =
        (const serial_state_t *)soc->serial_periph->state;
    add_bytes(&d, serial, sizeof *serial);

    const xbus_unknown1_state_t *unknown1 =
        (const xbus_unknown1_state_t *)soc->xbus_unknown1_periph->state;
    ADD(&d, unknown1->id);
    ADD(&d, unknown1->transaction_seq);
    ADD(&d, unknown1->active_transaction_id);
    ADD(&d, unknown1->phase);
    ADD(&d, unknown1->control);
    ADD(&d, unknown1->deadline);

    const battery_state_t *battery =
        (const battery_state_t *)soc->battery_periph->state;
    ADD(&d, battery->available);
    if (battery->available) {
        ADD(&d, battery->level);
        ADD(&d, battery->charging);
        ADD(&d, battery->millivolts);
        ADD(&d, battery->adc_raw);
        ADD(&d, battery->adc_result_word);
        ADD(&d, battery->samples);
        ADD(&d, battery->state_changes);
    }

    keypad_mutable_state_t keypad;
    cemu_keypad_capture_mutable(soc->keypad_periph->state, &keypad);
    ADD(&d, keypad.scans);
    ADD(&d, keypad.handled_scans);
    ADD(&d, keypad.results);
    ADD(&d, keypad.startup_releases);
    ADD(&d, keypad.pressed);
    ADD(&d, keypad.sampled_pressed);
    ADD(&d, keypad.last_command);
    ADD(&d, keypad.last_result);
    ADD(&d, keypad.startup_power_pending);
    ADD(&d, keypad.startup_result_tagged);
    ADD(&d, keypad.startup_result_read_mask);
    ADD(&d, keypad.matrix_scan_phase);
    ADD(&d, keypad.release_activity_pending);

    if (soc->memory.n_flash_chips > 1) ADD(&d, soc->memory.n_flash_chips);
    for (int chip_index = 0; chip_index < soc->memory.n_flash_chips; chip_index++) {
        const flash_state_t *flash_state =
            cemu_memory_controller_flash_state(&soc->memory, chip_index);
        ADD(&d, flash_state->kind);
        if (flash_state->kind == FLASH_MODEL_M58LW064D) {
            const m58lw064d_state_t *flash = &flash_state->u.m58;
            ADD(&d, flash->read_mode);
            ADD(&d, flash->command_phase);
            ADD(&d, flash->status);
            ADD(&d, flash->cmd_writes);
            ADD(&d, flash->erased_blocks);
            add_bytes(&d, flash->factory_uid, sizeof flash->factory_uid);
            ADD(&d, flash->factory_uid_set);
            ADD(&d, flash->write_buffer_state);
            ADD(&d, flash->write_buffer_words_left);
            ADD(&d, flash->write_buffer_block);
            ADD(&d, flash->write_buffer_page);
            ADD(&d, flash->write_buffer_len);
            for (size_t i = 0; i < flash->write_buffer_len; i++) {
                ADD(&d, flash->write_buffer[i].off);
                ADD(&d, flash->write_buffer[i].value);
                ADD(&d, flash->write_buffer[i].size);
            }
            ADD(&d, flash->store.count);
            for (size_t i = 0; i < flash->store.count; i++) {
                ADD(&d, flash->store.patches[i].off);
                ADD(&d, flash->store.patches[i].value);
            }
        } else if (flash_state->kind == FLASH_MODEL_AM29LV) {
            const am29lv_state_t *flash = &flash_state->u.am29;
            ADD(&d, flash->config->chip_size);
            ADD(&d, flash->config->sector_count);
            ADD(&d, flash->config->manufacturer_id);
            ADD(&d, flash->config->device_id1);
            ADD(&d, flash->config->device_id2);
            ADD(&d, flash->config->device_id3);
            ADD(&d, flash->mode);
            ADD(&d, flash->phase);
            ADD(&d, flash->cmd_writes);
            ADD(&d, flash->protected_start);
            ADD(&d, flash->protected_end);
            add_bytes(&d, flash->secsi, sizeof flash->secsi);
            ADD(&d, flash->secsi_esn_set);
            ADD(&d, flash->secsi_locked);
            ADD(&d, flash->write_buffer_sector);
            ADD(&d, flash->write_buffer_page);
            ADD(&d, flash->write_buffer_words_left);
            add_bytes(&d, flash->erased_sectors, sizeof flash->erased_sectors);
            ADD(&d, flash->erase_active);
            ADD(&d, flash->erase_suspended);
            ADD(&d, flash->erase_sector);
            ADD(&d, flash->erase_timer_ticks_remaining);
            ADD(&d, flash->erase_ticks_remaining);
            ADD(&d, flash->erase_toggle);
            ADD(&d, flash->write_buffer_len);
            for (size_t i = 0; i < flash->write_buffer_len; i++) {
                ADD(&d, flash->write_buffer[i].off);
                ADD(&d, flash->write_buffer[i].value);
                ADD(&d, flash->write_buffer[i].size);
            }
            ADD(&d, flash->store.count);
            for (size_t i = 0; i < flash->store.count; i++) {
                ADD(&d, flash->store.patches[i].off);
                ADD(&d, flash->store.patches[i].value);
            }
        } else if (flash_state->kind == FLASH_MODEL_W30) {
            const w30_state_t *flash = &flash_state->u.w30;
            add_bytes(&d, flash->mode,
                      flash->config->partition_count * sizeof flash->mode[0]);
            add_bytes(&d, flash->status,
                      flash->config->partition_count *
                          sizeof flash->status[0]);
            add_bytes(&d, flash->configuration,
                      flash->config->partition_count *
                          sizeof flash->configuration[0]);
            ADD(&d, flash->phase);
            ADD(&d, flash->setup_partition);
            ADD(&d, flash->setup_off);
            ADD(&d, flash->cmd_writes);
            add_bytes(&d, flash->erased_blocks,
                      cemu_w30_block_bitmap_words(flash) *
                          sizeof flash->erased_blocks[0]);
            add_bytes(&d, flash->locked_blocks,
                      cemu_w30_block_bitmap_words(flash) *
                          sizeof flash->locked_blocks[0]);
            add_bytes(&d, flash->lockdown_blocks,
                      cemu_w30_block_bitmap_words(flash) *
                          sizeof flash->lockdown_blocks[0]);
            add_bytes(&d, flash->factory_uid,
                      sizeof flash->factory_uid);
            ADD(&d, flash->factory_uid_set);
            add_bytes(&d, flash->customer, sizeof flash->customer);
            ADD(&d, flash->customer_locked);
            add_bytes(&d, &flash->active, sizeof flash->active);
            add_bytes(&d, &flash->suspended_erase,
                      sizeof flash->suspended_erase);
            add_bytes(&d, &flash->suspended_program,
                      sizeof flash->suspended_program);
            ADD(&d, flash->store.count);
            for (size_t i = 0; i < flash->store.count; i++) {
                ADD(&d, flash->store.patches[i].off);
                ADD(&d, flash->store.patches[i].value);
            }
        }
    }

    for (int i = 0; i < soc->n_peripherals; i++) {
        const peripheral_t *p = soc->peripherals[i];
        if (!strcmp(p->id, "gpt1")) add_bytes(&d, p->state, sizeof(gpt1_state_t));
        else if (!strcmp(p->id, "gpt2")) add_bytes(&d, p->state, sizeof(gpt2_state_t));
        else if (!strcmp(p->id, "capcom1")) add_bytes(&d, p->state, sizeof(capcom1_state_t));
        else if (!strcmp(p->id, "capcom2")) add_bytes(&d, p->state, sizeof(capcom2_state_t));
    }

    const ssc0_state_t *ssc = (const ssc0_state_t *)soc->ssc0_periph->state;
    ssc0_state_t ssc_copy = *ssc;
    ssc_copy.slave_start = NULL;
    ssc_copy.slave_complete = NULL;
    ssc_copy.slave_abort = NULL;
    ssc_copy.slave_ctx = NULL;
    add_bytes(&d, &ssc_copy, sizeof ssc_copy);

    if (soc->lcd_periph) {
        add_bytes(&d, soc->lcd_periph->model,
                  strlen(soc->lcd_periph->model) + 1u);
        add_bytes(&d, soc->lcd_periph->state,
                  cemu_lcd_state_size(soc->lcd_periph));
    }

    if (soc->twi_gpio_periph) {
        add_bytes(&d, soc->twi_gpio_periph->model,
                  strlen(soc->twi_gpio_periph->model) + 1u);
        add_bytes(&d, soc->twi_gpio_periph->state,
                  sizeof(twi_gpio_state_t));
    }

    const sim_state_t *sim = (const sim_state_t *)soc->sim_periph->state;
    sim_state_t sim_copy = *sim;
    sim_copy.restored = 0;
    for (size_t i = 0; i < SIM_PROFILE_FILE_CAP; i++) sim_copy.files[i].data = NULL;
    add_bytes(&d, &sim_copy, sizeof sim_copy);

    gsm_stub_state_t baseband = *(const gsm_stub_state_t *)
        soc->gsm_stub_periph->state;
    gsm_legacy_adapter_state_t legacy =
        *(const gsm_legacy_adapter_state_t *)
        soc->gsm_legacy_adapter_periph->state;
    /* Restore markers only defer CLI attachment until after snapshot load;
     * they are not machine state. */
    baseband.restored = 0;
    legacy.restored = 0;
    add_bytes(&d, &baseband, sizeof baseband);
    add_bytes(&d, &legacy, sizeof legacy);

    return d.value;
}

#undef ADD
