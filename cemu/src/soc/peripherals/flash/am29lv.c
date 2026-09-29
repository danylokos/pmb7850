#include "am29lv.h"

#include <stdlib.h>
#include <string.h>

#define AM29_UNLOCK1_WORD 0x555u
#define AM29_UNLOCK2_WORD 0x2AAu
#define AM29_FACTORY_SECSI_INDICATOR 0x0098u
#define AM29_WRITE_BUFFER_SIZE (AM29LV_WRITE_BUFFER_WORDS * 2u)

const am29lv_config_t cemu_AM29LV640MH_CONFIG = {
    .model = AM29LV640MH_MODEL,
    .chip_size = AM29LV640MH_CHIP_SIZE,
    .sector_count = AM29LV640MH_SECTOR_COUNT,
    .manufacturer_id = AM29LV_MANUFACTURER_ID,
    .device_id1 = AM29LV_DEVICE_ID1,
    .device_id2 = AM29LV640MH_DEVICE_ID2,
    .device_id3 = AM29LV640MH_DEVICE_ID3,
};

const am29lv_config_t cemu_AM29LV128MH_CONFIG = {
    .model = AM29LV128MH_MODEL,
    .chip_size = AM29LV128MH_CHIP_SIZE,
    .sector_count = AM29LV128MH_SECTOR_COUNT,
    .manufacturer_id = AM29LV_MANUFACTURER_ID,
    .device_id1 = AM29LV_DEVICE_ID1,
    .device_id2 = AM29LV128MH_DEVICE_ID2,
    .device_id3 = AM29LV128MH_DEVICE_ID3,
};

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int secsi_hex_parse(const char *text, uint8_t *data, size_t size) {
    if (!text || !data) return -1;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    if (strlen(text) != size * 2u) return -1;
    for (size_t i = 0; i < size; i++) {
        int hi = hex_nibble(text[2u * i]);
        int lo = hex_nibble(text[2u * i + 1u]);
        if (hi < 0 || lo < 0) return -1;
        data[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

int cemu_am29lv_secsi_esn_parse(const char *text,
                                uint8_t esn[AM29LV_ESN_SIZE]) {
    return secsi_hex_parse(text, esn, AM29LV_ESN_SIZE);
}

int cemu_am29lv_secsi_customer_parse(
        const char *text, uint8_t customer[AM29LV_CUSTOMER_SIZE]) {
    return secsi_hex_parse(text, customer, AM29LV_CUSTOMER_SIZE);
}

void cemu_am29lv_secsi_esn_set(am29lv_state_t *st,
                               const uint8_t esn[AM29LV_ESN_SIZE]) {
    if (!st || !esn) return;
    memcpy(st->secsi, esn, AM29LV_ESN_SIZE);
    st->secsi_esn_set = 1;
}

int cemu_am29lv_secsi_factory_set(am29lv_state_t *st, uint16_t offset,
                                  const uint8_t *data, size_t len) {
    if (!st || !data || offset > AM29LV_SECSI_SIZE ||
        len > AM29LV_SECSI_SIZE - offset)
        return 0;
    memcpy(st->secsi + offset, data, len);
    st->secsi_esn_set = 1;
    return 1;
}

static uint32_t secsi_offset(uint32_t off) {
    /* SecSi replaces the first 256 bytes of a sector. PMB7850 firmware
     * accesses that sector through its 0x800000 native flash aperture. */
    return off & (AM29LV_SECTOR_SIZE - 1u);
}

int cemu_am29lv_seed_bytes(const uint8_t *data, size_t len,
                           am29lv_state_t *st, uint32_t off,
                           const uint8_t *bytes, size_t count) {
    return data && st && len <= UINT32_MAX &&
           cemu_nor_store_seed(&st->store, (uint32_t)len, off, bytes, count);
}

uint8_t cemu_am29lv_array_read8(const uint8_t *data, size_t len,
                           const am29lv_state_t *st, uint32_t off) {
    if (off >= len) return 0xFF;
    uint32_t sector = off / AM29LV_SECTOR_SIZE;
    int erased = sector < st->config->sector_count &&
        (st->erased_sectors[sector >> 6] & (UINT64_C(1) << (sector & 63u)));
    return cemu_nor_store_read(&st->store, data, len, off, erased);
}

static int program_byte(const uint8_t *data, size_t len,
                        am29lv_state_t *st, uint32_t off, uint8_t value) {
    if (off >= len) return 1;
    uint32_t sector = off / AM29LV_SECTOR_SIZE;
    int erased = sector < st->config->sector_count &&
        (st->erased_sectors[sector >> 6] & (UINT64_C(1) << (sector & 63u)));
    return cemu_nor_store_program_byte(&st->store, data, len,
                                  st->config->chip_size,
                                  off, value, erased, NULL);
}

static void write_buffer_reset(am29lv_state_t *st) {
    st->write_buffer_sector = 0;
    st->write_buffer_page = 0;
    st->write_buffer_words_left = 0;
    st->write_buffer_len = 0;
}

static int write_buffer_push(am29lv_state_t *st, uint32_t off,
                             uint16_t value, int size) {
    if (size != 2 || (off & 1u) ||
        st->write_buffer_len >= AM29LV_WRITE_BUFFER_WORDS ||
        (off & ~(AM29LV_SECTOR_SIZE - 1u)) != st->write_buffer_sector ||
        (off & ~(AM29_WRITE_BUFFER_SIZE - 1u)) != st->write_buffer_page)
        return 0;
    am29lv_write_buffer_entry_t *entry =
        &st->write_buffer[st->write_buffer_len++];
    entry->off = off;
    entry->value = value;
    entry->size = (uint8_t)size;
    return 1;
}

static void write_buffer_commit(const uint8_t *data, size_t len,
                                am29lv_state_t *st) {
    for (size_t i = 0; i < st->write_buffer_len; i++) {
        const am29lv_write_buffer_entry_t *entry = &st->write_buffer[i];
        (void)program_byte(data, len, st, entry->off, (uint8_t)entry->value);
        if (entry->size == 2)
            (void)program_byte(data, len, st, entry->off + 1u,
                               (uint8_t)(entry->value >> 8));
    }
}

static int address_is_protected(const am29lv_state_t *st, uint32_t off) {
    return st->protected_end > st->protected_start &&
           off >= st->protected_start && off < st->protected_end;
}

static void access_reset(am29lv_access_t *access, const char *detail) {
    if (!access) return;
    access->subtype = NULL;
    access->detail = detail;
    access->include_mode = 0;
    access->mutation_count = 0;
}

const char *cemu_am29lv_mode_str(const am29lv_state_t *st) {
    if (!st) return "invalid";
    if (st->mode == AM29_MODE_AUTOSELECT) return "autoselect";
    if (st->mode == AM29_MODE_SECSI) return "secsi";
    return "array";
}

const char *cemu_am29lv_phase_str(am29lv_phase_t phase) {
    switch (phase) {
        case AM29_PHASE_EXPECT_55: return "expect-55";
        case AM29_PHASE_EXPECT_COMMAND: return "expect-command";
        case AM29_PHASE_PROGRAM: return "program";
        case AM29_PHASE_SECSI_EXIT_ZERO: return "secsi-exit-zero";
        case AM29_PHASE_SECSI_PROTECT_40: return "secsi-protect-40";
        case AM29_PHASE_WRITE_BUFFER_COUNT: return "write-buffer-count";
        case AM29_PHASE_WRITE_BUFFER_DATA: return "write-buffer-data";
        case AM29_PHASE_WRITE_BUFFER_CONFIRM: return "write-buffer-confirm";
        case AM29_PHASE_ERASE_UNLOCK_AA: return "erase-unlock-aa";
        case AM29_PHASE_ERASE_UNLOCK_55: return "erase-unlock-55";
        case AM29_PHASE_ERASE_CONFIRM: return "erase-confirm";
        default: return "idle";
    }
}

static uint16_t autoselect_word(const am29lv_state_t *st, uint32_t off) {
    uint32_t word = (off >> 1) & 0xFFFu;
    if (word == 0x000) return st->config->manufacturer_id;
    if (word == 0x001) return st->config->device_id1;
    if (word == 0x00E) return st->config->device_id2;
    if (word == 0x00F) return st->config->device_id3;
    if (word == 0x003) return st->secsi_locked
                                ? AM29_FACTORY_SECSI_INDICATOR : 0x0000u;
    /* Datasheet protection selector X02 is byte offset +4. M55 firmware reads
     * byte offset +8; retain that observed PMB-visible alias until bus evidence
     * distinguishes the selector wiring. */
    if ((off & (AM29LV_SECTOR_SIZE - 1u)) == 0x0004u ||
        (off & (AM29LV_SECTOR_SIZE - 1u)) == 0x0008u)
        return address_is_protected(st, off) ? 0x0001u : 0x0000u;
    return 0x0000u;
}

static uint8_t read8_impl(const uint8_t *data, size_t len,
                          am29lv_state_t *st, uint32_t off,
                          int command_visible, am29lv_access_t *access,
                          int advance_status) {
    access_reset(access, "flash");
    if (command_visible && st->erase_active && !(off & 1u) &&
        (!st->erase_suspended ||
         (off & ~(AM29LV_SECTOR_SIZE - 1u)) == st->erase_sector)) {
        uint32_t sector = off & ~(AM29LV_SECTOR_SIZE - 1u);
        if (advance_status) st->erase_toggle ^= 1;
        uint8_t value = 0;
        if (st->erase_suspended) value |= 0x80u;
        if (!st->erase_suspended && st->erase_toggle) value |= 0x40u;
        if (sector == st->erase_sector && st->erase_toggle) value |= 0x04u;
        if (!st->erase_timer_ticks_remaining) value |= 0x08u;
        if (access) {
            access->subtype = "status";
            access->detail = "sector-erase-status";
            access->include_mode = 1;
        }
        return value;
    }
    if (!command_visible || st->mode == AM29_MODE_ARRAY)
        return cemu_am29lv_array_read8(data, len, st, off);
    if (st->mode == AM29_MODE_SECSI) {
        uint32_t secsi_off = secsi_offset(off);
        if (access) {
            access->subtype = "secsi";
            access->detail = "secsi";
        }
        return secsi_off < AM29LV_SECSI_SIZE
             ? st->secsi[secsi_off] : 0xFF;
    }
    uint16_t value = autoselect_word(st, off);
    if (access) {
        access->subtype = "id";
        access->detail =
            ((off & (AM29LV_SECTOR_SIZE - 1u)) == 4u ||
             (off & (AM29LV_SECTOR_SIZE - 1u)) == 8u)
                       ? "protection" : "identity";
    }
    return (off & 1u) ? (uint8_t)(value >> 8) : (uint8_t)value;
}

uint8_t cemu_am29lv_read8(const uint8_t *data, size_t len,
                          am29lv_state_t *st, uint32_t off,
                          int command_visible, am29lv_access_t *access) {
    return read8_impl(data, len, st, off, command_visible, access, 1);
}

uint8_t cemu_am29lv_peek8(const uint8_t *data, size_t len,
                          const am29lv_state_t *st, uint32_t off,
                          int command_visible) {
    return read8_impl(data, len, (am29lv_state_t *)st, off,
                      command_visible, NULL, 0);
}

static const char *command_name(uint8_t command) {
    switch (command) {
        case 0x90: return "autoselect";
        case 0x88: return "secsi-enter";
        case 0xA0: return "program";
        case 0x60: return "secsi-protect";
        case 0x40: return "secsi-protect-confirm";
        case 0x25: return "write-buffer";
        case 0x29: return "write-buffer-confirm";
        case 0x00: return "secsi-exit";
        case 0xF0: return "reset";
        case 0x80: return "erase-setup";
        case 0x30: return "sector-erase-confirm";
        case 0x10: return "chip-erase-confirm";
        case 0xB0: return "erase-suspend";
        default: return "unknown";
    }
}

static void bus_write(const uint8_t *data, size_t len,
                      am29lv_state_t *st, uint32_t off,
                      uint16_t value, int size, int command_visible,
                      am29lv_access_t *access) {
    uint8_t command = value & 0xFFu;
    uint32_t word = (off >> 1) & 0x7FFu;
    access_reset(access, "unknown");
    if (access) access->include_mode = 1;
    if (!command_visible) {
        if (access) access->subtype = "write-absorbed";
        return;
    }

    if (st->erase_active && !st->erase_suspended && command == 0xB0) {
        st->cmd_writes++;
        st->erase_suspended = 1;
        st->erase_timer_ticks_remaining = 0;
        st->erase_toggle = 0;
        if (access) {
            access->subtype = "cmd";
            access->detail = "erase-suspend";
        }
        return;
    }

    if (st->erase_active && st->erase_suspended &&
        st->mode == AM29_MODE_ARRAY && st->phase == AM29_PHASE_IDLE &&
        command == 0x30 &&
        (off & ~(AM29LV_SECTOR_SIZE - 1u)) == st->erase_sector) {
        st->cmd_writes++;
        st->erase_suspended = 0;
        st->erase_toggle = 0;
        if (access) {
            access->subtype = "cmd";
            access->detail = "erase-resume";
        }
        return;
    }
    if (st->erase_active && st->erase_suspended &&
        st->mode == AM29_MODE_ARRAY && st->phase == AM29_PHASE_IDLE &&
        command == 0x30) {
        if (access) {
            access->subtype = "write-absorbed";
            access->detail = "erase-resume-wrong-sector";
        }
        return;
    }

    if (st->erase_active && !st->erase_suspended) {
        if (access) {
            access->subtype = "write-absorbed";
            access->detail = "erase-busy-write-absorbed";
        }
        return;
    }

    if (st->phase == AM29_PHASE_WRITE_BUFFER_COUNT) {
        uint16_t count = value & 0xFFFFu;
        if (count < AM29LV_WRITE_BUFFER_WORDS) {
            st->write_buffer_words_left = (uint8_t)(count + 1u);
            st->phase = AM29_PHASE_WRITE_BUFFER_DATA;
            if (access) {
                access->subtype = "cmd";
                access->detail = "write-buffer-count";
            }
        } else {
            st->phase = AM29_PHASE_IDLE;
            write_buffer_reset(st);
            if (access) {
                access->subtype = "write-absorbed";
                access->detail = "write-buffer-count-invalid";
            }
        }
        return;
    }

    if (st->phase == AM29_PHASE_WRITE_BUFFER_DATA) {
        if (!write_buffer_push(st, off, value, size)) {
            st->phase = AM29_PHASE_IDLE;
            write_buffer_reset(st);
            if (access) {
                access->subtype = "write-absorbed";
                access->detail = "write-buffer-data-invalid";
            }
            return;
        }
        if (st->write_buffer_words_left) st->write_buffer_words_left--;
        if (!st->write_buffer_words_left)
            st->phase = AM29_PHASE_WRITE_BUFFER_CONFIRM;
        if (access) {
            access->subtype = "program";
            access->detail = "write-buffer-data";
        }
        return;
    }

    if (st->phase == AM29_PHASE_WRITE_BUFFER_CONFIRM) {
        st->cmd_writes++;
        int valid = command == 0x29 &&
                    (off & ~(AM29LV_SECTOR_SIZE - 1u)) ==
                    st->write_buffer_sector;
        if (valid) {
            write_buffer_commit(data, len, st);
            if (access) {
                for (size_t i = 0; i < st->write_buffer_len; i++) {
                    const am29lv_write_buffer_entry_t *entry =
                        &st->write_buffer[i];
                    flash_access_add_mutation(
                        access, FLASH_MUTATION_PROGRAM, entry->off,
                        entry->size);
                }
            }
        }
        st->phase = AM29_PHASE_IDLE;
        write_buffer_reset(st);
        if (access) {
            access->subtype = valid ? "program" : "write-absorbed";
            access->detail = valid ? "write-buffer-confirm"
                                   : "write-buffer-confirm-invalid";
        }
        return;
    }

    if (st->phase == AM29_PHASE_PROGRAM) {
        int main_array = st->mode != AM29_MODE_SECSI &&
            (!st->erase_active ||
             (off & ~(AM29LV_SECTOR_SIZE - 1u)) != st->erase_sector);
        if (st->mode == AM29_MODE_SECSI) {
            uint32_t secsi_off = secsi_offset(off);
            if (!st->secsi_locked && secsi_off < AM29LV_SECSI_SIZE) {
                st->secsi[secsi_off] &= (uint8_t)value;
                if (size == 2 &&
                    secsi_off + 1u < AM29LV_SECSI_SIZE)
                    st->secsi[secsi_off + 1u] &= (uint8_t)(value >> 8);
            }
        } else if (!st->erase_active ||
                   (off & ~(AM29LV_SECTOR_SIZE - 1u)) != st->erase_sector) {
            (void)program_byte(data, len, st, off, (uint8_t)value);
            if (size == 2)
                (void)program_byte(data, len, st, off + 1u,
                                   (uint8_t)(value >> 8));
            st->mode = AM29_MODE_ARRAY;
        } else {
            st->mode = AM29_MODE_ARRAY;
        }
        st->phase = AM29_PHASE_IDLE;
        if (access) {
            int selected = st->erase_active &&
                (off & ~(AM29LV_SECTOR_SIZE - 1u)) == st->erase_sector;
            access->subtype = selected ? "write-absorbed" : "program";
            access->detail = selected
                           ? "program-in-suspended-erase-sector"
                           : (st->mode == AM29_MODE_SECSI
                              ? (st->secsi_locked ? "secsi-locked"
                                                  : "secsi-program")
                              : "program");
            if (main_array)
                flash_access_add_mutation(
                    access, FLASH_MUTATION_PROGRAM, off, (uint32_t)size);
        }
        return;
    }

    if (command == 0xF0) {
        st->cmd_writes++;
        st->mode = AM29_MODE_ARRAY;
        st->phase = AM29_PHASE_IDLE;
        write_buffer_reset(st);
        if (access) {
            access->subtype = "cmd";
            access->detail = "reset";
        }
        return;
    }

    if (st->phase == AM29_PHASE_SECSI_EXIT_ZERO) {
        st->cmd_writes++;
        if (command == 0x00) {
            st->mode = AM29_MODE_ARRAY;
            st->phase = AM29_PHASE_IDLE;
        } else {
            st->phase = AM29_PHASE_IDLE;
        }
        if (access) {
            access->subtype = "cmd";
            access->detail = command_name(command);
        }
        return;
    }

    if (st->phase == AM29_PHASE_SECSI_PROTECT_40) {
        st->cmd_writes++;
        if (command == 0x40 && !st->secsi_locked) st->secsi_locked = 1;
        st->phase = AM29_PHASE_IDLE;
        if (access) {
            access->subtype = "cmd";
            access->detail = st->secsi_locked && command == 0x40
                           ? "secsi-protect-confirm" : "unknown";
        }
        return;
    }

    if (st->phase == AM29_PHASE_ERASE_UNLOCK_AA) {
        st->cmd_writes++;
        st->phase = word == AM29_UNLOCK1_WORD && command == 0xAA
                  ? AM29_PHASE_ERASE_UNLOCK_55 : AM29_PHASE_IDLE;
        if (access) {
            access->subtype = st->phase == AM29_PHASE_ERASE_UNLOCK_55
                            ? "unlock" : "write-absorbed";
            access->detail = st->phase == AM29_PHASE_ERASE_UNLOCK_55
                           ? "erase-unlock-aa" : "erase-unlock-aa-rejected";
        }
        return;
    }

    if (st->phase == AM29_PHASE_ERASE_UNLOCK_55) {
        st->cmd_writes++;
        st->phase = word == AM29_UNLOCK2_WORD && command == 0x55
                  ? AM29_PHASE_ERASE_CONFIRM : AM29_PHASE_IDLE;
        if (access) {
            access->subtype = st->phase == AM29_PHASE_ERASE_CONFIRM
                            ? "unlock" : "write-absorbed";
            access->detail = st->phase == AM29_PHASE_ERASE_CONFIRM
                           ? "erase-unlock-55" : "erase-unlock-55-rejected";
        }
        return;
    }

    if (st->phase == AM29_PHASE_ERASE_CONFIRM) {
        st->cmd_writes++;
        uint32_t sector = off & ~(AM29LV_SECTOR_SIZE - 1u);
        int valid = command == 0x30 &&
                    sector / AM29LV_SECTOR_SIZE <
                        st->config->sector_count &&
                    !address_is_protected(st, off);
        st->phase = AM29_PHASE_IDLE;
        if (valid) {
            st->mode = AM29_MODE_ARRAY;
            st->erase_active = 1;
            st->erase_suspended = 0;
            st->erase_sector = sector;
            st->erase_timer_ticks_remaining = AM29LV_ERASE_TIMER_TICKS;
            st->erase_ticks_remaining = AM29LV_SECTOR_ERASE_TICKS;
            st->erase_toggle = 0;
        }
        if (access) {
            access->subtype = valid ? "cmd" : "write-absorbed";
            access->detail = valid ? "sector-erase-confirm"
                                   : (command == 0x30
                                      ? "sector-erase-protected"
                                      : "erase-confirm-unsupported");
        }
        return;
    }

    if (st->phase == AM29_PHASE_EXPECT_55) {
        st->cmd_writes++;
        st->phase = word == AM29_UNLOCK2_WORD && command == 0x55
                  ? AM29_PHASE_EXPECT_COMMAND : AM29_PHASE_IDLE;
        if (access) {
            access->subtype = "unlock";
            access->detail = st->phase == AM29_PHASE_EXPECT_COMMAND
                           ? "unlock-55" : "unlock-rejected";
        }
        return;
    }

    if (st->phase == AM29_PHASE_EXPECT_COMMAND) {
        st->cmd_writes++;
        st->phase = AM29_PHASE_IDLE;
        int accepted = 0;
        if (command == 0x25 && st->mode == AM29_MODE_ARRAY &&
            (!st->erase_active ||
             (off & ~(AM29LV_SECTOR_SIZE - 1u)) != st->erase_sector)) {
            st->write_buffer_sector =
                off & ~(AM29LV_SECTOR_SIZE - 1u);
            st->write_buffer_page =
                off & ~(AM29_WRITE_BUFFER_SIZE - 1u);
            st->write_buffer_words_left = 0;
            st->write_buffer_len = 0;
            st->phase = AM29_PHASE_WRITE_BUFFER_COUNT;
            accepted = 1;
        } else if (word == AM29_UNLOCK1_WORD) {
            if (command == 0x90) {
                if (st->mode == AM29_MODE_SECSI)
                    st->phase = AM29_PHASE_SECSI_EXIT_ZERO;
                else
                    st->mode = AM29_MODE_AUTOSELECT;
                accepted = 1;
            } else if (command == 0x88 && !st->erase_active) {
                st->mode = AM29_MODE_SECSI;
                accepted = 1;
            } else if (command == 0xA0) {
                st->phase = AM29_PHASE_PROGRAM;
                accepted = 1;
            } else if (command == 0x80 && st->mode == AM29_MODE_ARRAY &&
                       !st->erase_active) {
                st->phase = AM29_PHASE_ERASE_UNLOCK_AA;
                accepted = 1;
            }
        }
        if (access) {
            access->subtype = accepted ? "cmd" : "write-absorbed";
            access->detail = command_name(command);
        }
        return;
    }

    if (st->mode == AM29_MODE_SECSI && command == 0x60) {
        st->cmd_writes++;
        st->phase = AM29_PHASE_SECSI_PROTECT_40;
        if (access) {
            access->subtype = "cmd";
            access->detail = "secsi-protect";
        }
        return;
    }

    if (word == AM29_UNLOCK1_WORD && command == 0xAA) {
        st->cmd_writes++;
        st->phase = AM29_PHASE_EXPECT_55;
        if (access) {
            access->subtype = "unlock";
            access->detail = "unlock-aa";
        }
        return;
    }

    if (access) {
        access->subtype = "write-absorbed";
        access->detail = "unknown";
    }
}

void cemu_am29lv_write8(const uint8_t *data, size_t len,
                        am29lv_state_t *st, uint32_t off, uint8_t value,
                        int command_visible, am29lv_access_t *access) {
    bus_write(data, len, st, off, value, 1, command_visible, access);
}

void cemu_am29lv_write16(const uint8_t *data, size_t len,
                         am29lv_state_t *st, uint32_t off, uint16_t value,
                         int command_visible, am29lv_access_t *access) {
    bus_write(data, len, st, off, value, 2, command_visible, access);
}

void cemu_am29lv_reset(am29lv_state_t *st) {
    if (!st) return;
    st->mode = AM29_MODE_ARRAY;
    st->phase = AM29_PHASE_IDLE;
    st->erase_active = 0;
    st->erase_suspended = 0;
    st->erase_sector = 0;
    st->erase_timer_ticks_remaining = 0;
    st->erase_ticks_remaining = 0;
    st->erase_toggle = 0;
    write_buffer_reset(st);
}

static void erase_complete(am29lv_state_t *st) {
    uint32_t sector = st->erase_sector / AM29LV_SECTOR_SIZE;
    uint32_t end = st->erase_sector + AM29LV_SECTOR_SIZE;
    cemu_nor_store_erase_range(&st->store, st->erase_sector, end);
    st->erased_sectors[sector >> 6] |= UINT64_C(1) << (sector & 63u);
    st->erase_active = 0;
    st->erase_suspended = 0;
    st->erase_timer_ticks_remaining = 0;
    st->erase_ticks_remaining = 0;
    st->erase_toggle = 0;
}

uint32_t cemu_am29lv_tick(am29lv_state_t *st, uint32_t ticks) {
    uint32_t events = AM29_TICK_NO_EVENT;
    if (!st || !st->erase_active || st->erase_suspended || !ticks)
        return events;
    while (ticks-- && st->erase_active) {
        if (st->erase_timer_ticks_remaining &&
            --st->erase_timer_ticks_remaining == 0)
            events |= AM29_TICK_ERASE_TIMER;
        if (st->erase_ticks_remaining && --st->erase_ticks_remaining == 0) {
            erase_complete(st);
            events |= AM29_TICK_ERASE_COMPLETE;
        }
    }
    return events;
}

uint64_t cemu_am29lv_next_event_ticks(const am29lv_state_t *st) {
    if (!st || !st->erase_active || st->erase_suspended) return UINT64_MAX;
    if (st->erase_timer_ticks_remaining)
        return st->erase_timer_ticks_remaining;
    return st->erase_ticks_remaining ? st->erase_ticks_remaining : 1;
}

void cemu_am29lv_advance_quiet(am29lv_state_t *st, uint64_t ticks) {
    if (!st || !st->erase_active || st->erase_suspended || !ticks) return;
    if (st->erase_timer_ticks_remaining)
        st->erase_timer_ticks_remaining -= (uint32_t)ticks;
    st->erase_ticks_remaining -= (uint32_t)ticks;
}

void cemu_am29lv_configure(am29lv_state_t *st,
                           uint32_t protected_start, uint32_t protected_size,
                           int secsi_factory_locked) {
    if (!st) return;
    st->protected_start = protected_start;
    st->protected_end = protected_start + protected_size;
    st->secsi_locked = secsi_factory_locked != 0;
}

void cemu_am29lv_state_free(am29lv_state_t *st) {
    if (!st) return;
    cemu_nor_store_free(&st->store);
    memset(st, 0, sizeof(*st));
}

int cemu_am29lv_state_restore(am29lv_state_t *st,
                              am29lv_mode_t mode,
                              am29lv_phase_t phase,
                              uint32_t cmd_writes,
                              const uint8_t secsi[AM29LV_SECSI_SIZE],
                              int secsi_esn_set, int secsi_locked,
                              uint32_t write_buffer_sector,
                              uint32_t write_buffer_page,
                              uint8_t write_buffer_words_left,
                              const am29lv_write_buffer_entry_t *write_buffer,
                              size_t write_buffer_len,
                              const uint64_t
                                  erased_sectors[AM29LV_MAX_SECTOR_COUNT / 64u],
                              int erase_active, int erase_suspended,
                              uint32_t erase_sector,
                              uint32_t erase_timer_ticks_remaining,
                              uint32_t erase_ticks_remaining, int erase_toggle,
                              const am29lv_patch_t *patches,
                              size_t patch_count) {
    if (!st || !st->config || !st->config->model ||
        !st->config->chip_size || !st->config->sector_count ||
        st->config->sector_count > AM29LV_MAX_SECTOR_COUNT ||
        write_buffer_len > AM29LV_WRITE_BUFFER_WORDS ||
        (erase_suspended && !erase_active) ||
        (erase_active &&
         ((erase_sector & (AM29LV_SECTOR_SIZE - 1u)) ||
          erase_sector / AM29LV_SECTOR_SIZE >=
              st->config->sector_count ||
          !erase_ticks_remaining ||
          erase_ticks_remaining > AM29LV_SECTOR_ERASE_TICKS ||
          erase_timer_ticks_remaining > AM29LV_ERASE_TIMER_TICKS ||
          (erase_suspended && erase_timer_ticks_remaining))))
        return 0;
    am29lv_state_t tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.config = st->config;
    tmp.mode = mode;
    tmp.phase = phase;
    tmp.cmd_writes = cmd_writes;
    tmp.protected_start = st->protected_start;
    tmp.protected_end = st->protected_end;
    memcpy(tmp.secsi, secsi ? secsi : st->secsi, sizeof tmp.secsi);
    tmp.secsi_esn_set = secsi_esn_set;
    tmp.secsi_locked = secsi_locked;
    tmp.write_buffer_sector = write_buffer_sector;
    tmp.write_buffer_page = write_buffer_page;
    tmp.write_buffer_words_left = write_buffer_words_left;
    if (erased_sectors)
        memcpy(tmp.erased_sectors, erased_sectors, sizeof tmp.erased_sectors);
    tmp.erase_active = erase_active;
    tmp.erase_suspended = erase_suspended;
    tmp.erase_sector = erase_sector;
    tmp.erase_timer_ticks_remaining = erase_timer_ticks_remaining;
    tmp.erase_ticks_remaining = erase_ticks_remaining;
    tmp.erase_toggle = erase_toggle;
    if (write_buffer_len) {
        if (!write_buffer) return 0;
        memcpy(tmp.write_buffer, write_buffer,
               write_buffer_len * sizeof(*tmp.write_buffer));
        tmp.write_buffer_len = write_buffer_len;
    }
    if (!cemu_nor_store_restore(&tmp.store, patches, patch_count,
                           tmp.config->chip_size))
        return 0;
    cemu_am29lv_state_free(st);
    *st = tmp;
    return 1;
}

int cemu_am29lv_state_copy(am29lv_state_t *dst,
                      const am29lv_state_t *src) {
    if (!dst || !src) return 0;
    dst->config = src->config;
    dst->protected_start = src->protected_start;
    dst->protected_end = src->protected_end;
    return cemu_am29lv_state_restore(dst, src->mode, src->phase,
                                     src->cmd_writes, src->secsi,
                                     src->secsi_esn_set, src->secsi_locked,
                                     src->write_buffer_sector,
                                     src->write_buffer_page,
                                     src->write_buffer_words_left,
                                     src->write_buffer,
                                     src->write_buffer_len,
                                     src->erased_sectors,
                                     src->erase_active, src->erase_suspended,
                                     src->erase_sector,
                                     src->erase_timer_ticks_remaining,
                                     src->erase_ticks_remaining,
                                     src->erase_toggle,
                                     src->store.patches, src->store.count);
}

const am29lv_patch_t *
cemu_am29lv_state_patches(const am29lv_state_t *st, size_t *count_out) {
    return st ? cemu_nor_store_patches(&st->store, count_out) : NULL;
}

void cemu_am29lv_periph_init(peripheral_t *p, am29lv_state_t *st,
                        const am29lv_config_t *config) {
    memset(p, 0, sizeof(*p));
    memset(st, 0, sizeof(*st));
    memset(st->secsi, 0xFF, sizeof st->secsi);
    st->config = config;
    st->mode = AM29_MODE_ARRAY;
    p->id = "flash";
    p->model = cemu_am29lv_model_str(st);
    p->state = st;
}

void cemu_am29lv640mh_periph_init(peripheral_t *p, am29lv_state_t *st) {
    cemu_am29lv_periph_init(p, st, &cemu_AM29LV640MH_CONFIG);
}

void cemu_am29lv128mh_periph_init(peripheral_t *p, am29lv_state_t *st) {
    cemu_am29lv_periph_init(p, st, &cemu_AM29LV128MH_CONFIG);
}

const char *cemu_am29lv_model_str(const am29lv_state_t *st) {
    return st && st->config && st->config->model
         ? st->config->model : "invalid";
}
