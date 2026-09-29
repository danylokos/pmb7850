/* M58LW064D NOR flash command interface — see m58lw064d.h. */
#include "m58lw064d.h"
#include "flash.h"

#include <stdlib.h>
#include <string.h>

#define FLASH_MFR_CODE     0x0020u   /* ST/Numonyx manufacturer (Table 7) */
#define FLASH_DEVICE_CODE  0x0017u   /* M58LW064D device code */
#define FLASH_STATUS_READY 0x0080u   /* SR7=1 (ready) */
#define FLASH_STATUS_ERASE_ERROR   0x0020u
#define FLASH_STATUS_PROGRAM_ERROR 0x0010u
#define FLASH_STATUS_SEQUENCE_ERROR \
    (FLASH_STATUS_READY | FLASH_STATUS_ERASE_ERROR | FLASH_STATUS_PROGRAM_ERROR)
#define FLASH_PR_LOCK_BYTE 0x00FEu   /* PR word 0x80: factory lock bit0=0, user bit1=1 */
#define FLASH_INVALID_PAGE UINT32_MAX
#define FLASH_CHIP_SIZE \
    ((uint32_t)(M58LW064D_BLOCK_SIZE * M58LW064D_BLOCK_COUNT))

int cemu_m58lw064d_factory_uid_parse(const char *text, uint8_t uid[8]) {
    return cemu_flash_hex_parse(text, uid, 8);
}

void cemu_m58lw064d_factory_uid_set(m58lw064d_state_t *st, const uint8_t uid[8]) {
    if (!st || !uid) return;
    memcpy(st->factory_uid, uid, sizeof(st->factory_uid));
    st->factory_uid_set = 1;
}

/* ---- command interface ------------------------------------------------- */
static int flash_reserve_bytes(void **ptr, size_t *cap, size_t need, size_t elem_size) {
    if (*cap >= need) return 1;
    size_t new_cap = *cap ? *cap : 8;
    while (new_cap < need) new_cap *= 2u;
    void *new_ptr = realloc(*ptr, new_cap * elem_size);
    if (!new_ptr) return 0;
    *ptr = new_ptr;
    *cap = new_cap;
    return 1;
}

static void flash_write_buffer_reset(m58lw064d_state_t *st) {
    st->write_buffer_state = FLASH_WB_IDLE;
    st->write_buffer_words_left = 0;
    st->write_buffer_block = 0;
    st->write_buffer_page = FLASH_INVALID_PAGE;
    st->write_buffer_len = 0;
}

static void flash_command_reset(m58lw064d_state_t *st) {
    st->command_phase = FLASH_PHASE_IDLE;
    flash_write_buffer_reset(st);
}

int cemu_m58lw064d_seed_bytes(const uint8_t *data, size_t len,
                         m58lw064d_state_t *st, uint32_t off,
                         const uint8_t *bytes, size_t count) {
    return data && st && len <= FLASH_CHIP_SIZE &&
           cemu_nor_store_seed(&st->store, (uint32_t)len, off, bytes, count);
}

uint8_t cemu_m58lw064d_array_read8(const uint8_t *data, size_t len,
                              const m58lw064d_state_t *st, uint32_t off) {
    if (off >= len) return 0xFF;
    uint32_t block = off / M58LW064D_BLOCK_SIZE;
    int erased = block < M58LW064D_BLOCK_COUNT &&
                 (st->erased_blocks & (UINT64_C(1) << block));
    return cemu_nor_store_read(&st->store, data, len, off, erased);
}

static int flash_patch_program_byte(const uint8_t *data, size_t len,
                                    m58lw064d_state_t *st, uint32_t off,
                                    uint8_t value, int *program_error) {
    uint32_t block = off / M58LW064D_BLOCK_SIZE;
    int erased = block < M58LW064D_BLOCK_COUNT &&
                 (st->erased_blocks & (UINT64_C(1) << block));
    return cemu_nor_store_program_byte(&st->store, data, len, FLASH_CHIP_SIZE,
                                  off, value, erased, program_error);
}

static int flash_program_value(const uint8_t *data, size_t len,
                               m58lw064d_state_t *st, uint32_t off,
                               uint16_t value, int size,
                               int *program_error) {
    if (size != 1 && size != 2) return 0;
    if (off >= len || (size == 2 && (off + 1u >= len))) return 0;
    if (!flash_patch_program_byte(data, len, st, off,
                                  (uint8_t)(value & 0xFF),
                                  program_error))
        return 0;
    if (size == 2 &&
            !flash_patch_program_byte(data, len, st, off + 1u,
                                      (uint8_t)((value >> 8) & 0xFF),
                                      program_error))
        return 0;
    return 1;
}

static int flash_erase_block(size_t len, m58lw064d_state_t *st,
                             uint32_t off) {
    if (off >= len) return 0;
    uint32_t block = off / M58LW064D_BLOCK_SIZE;
    if (block >= M58LW064D_BLOCK_COUNT) return 0;
    uint32_t start = block * M58LW064D_BLOCK_SIZE;
    uint32_t end = start + M58LW064D_BLOCK_SIZE;
    cemu_nor_store_erase_range(&st->store, start, end);
    st->erased_blocks |= UINT64_C(1) << block;
    return 1;
}

static int flash_write_buffer_push(m58lw064d_state_t *st, uint32_t off,
                                   uint16_t value, int size) {
    if (!flash_reserve_bytes((void **)&st->write_buffer, &st->write_buffer_cap,
                             st->write_buffer_len + 1u, sizeof(*st->write_buffer))) return 0;
    st->write_buffer[st->write_buffer_len].off = off;
    st->write_buffer[st->write_buffer_len].value = value;
    st->write_buffer[st->write_buffer_len].size = (uint8_t)size;
    st->write_buffer_len++;
    return 1;
}

static int flash_commit_write_buffer(const uint8_t *data, size_t len,
                                     m58lw064d_state_t *st,
                                     int *program_error) {
    for (size_t i = 0; i < st->write_buffer_len; i++) {
        const m58lw064d_write_buffer_entry_t *entry = &st->write_buffer[i];
        if (!flash_program_value(data, len, st, entry->off, entry->value,
                                 entry->size, program_error))
            return 0;
    }
    return 1;
}

static const char *flash_read_subtype(const m58lw064d_state_t *st) {
    switch (st->read_mode) {
        case FLASH_ID: return "id";
        case FLASH_STATUS: return "status";
        case FLASH_CFI: return "cfi";
        default: return NULL;
    }
}

static const char *flash_read_detail(const m58lw064d_state_t *st, uint32_t off) {
    off &= 0xFFFF;
    if (st->read_mode != FLASH_ID) return "flash";
    if (off >= 0x0100 && off <= 0x0101) return "protection-lock";
    if (off >= 0x0102 && off <= 0x0109) return "factory-uid";
    return "flash";
}

static const char *flash_command_subtype(void) {
    return "cmd";
}

static const char *flash_absorbed_write_subtype(void) {
    return "write-absorbed";
}

const char *cemu_m58lw064d_mode_str(const m58lw064d_state_t *st) {
    switch (st->read_mode) {
        case FLASH_ID: return "id";
        case FLASH_STATUS: return "status";
        case FLASH_CFI: return "cfi";
        default: return "array";
    }
}

const char *cemu_m58lw064d_command_phase_str(m58lw064d_command_phase_t phase) {
    switch (phase) {
        case FLASH_PHASE_PROGRAM_DATA: return "program-data";
        case FLASH_PHASE_ERASE_CONFIRM: return "erase-confirm";
        default: return "idle";
    }
}

const char *cemu_m58lw064d_write_buffer_state_str(m58lw064d_write_buffer_state_t state) {
    switch (state) {
        case FLASH_WB_EXPECT_COUNT: return "expect-count";
        case FLASH_WB_LOADING: return "loading";
        case FLASH_WB_EXPECT_CONFIRM: return "expect-confirm";
        default: return "idle";
    }
}

static const char *flash_cmd_name(uint8_t cmd) {
    switch (cmd) {
        case 0xFF: return "read-array";
        case 0x90: return "read-id";
        case 0x70: return "read-status";
        case 0x98: return "read-query";
        case 0x50: return "clear-status";
        case 0x40:
        case 0x10: return "program";
        case 0x20: return "block-erase";
        case 0xD0: return "confirm/resume";
        case 0xE8: return "buffer-program";
        case 0xB0: return "suspend";
        case 0x60: return "block-protect";
        case 0xC0: return "protreg-program";
        case 0xB8: return "configure-sts";
        default: return "unknown";
    }
}

static uint16_t flash_signature_word(m58lw064d_state_t *st, uint32_t off) {
    if (st->read_mode == FLASH_STATUS) return st->status;
    if (st->read_mode == FLASH_ID) {
        off &= 0xFFFF;
        if (off == 0x0000) return FLASH_MFR_CODE;
        if (off == 0x0002) return FLASH_DEVICE_CODE;
        if (off == 0x0004) return 0x0000;   /* block protection: unprotected */
        /* Table 7 / p.18: PR word 0x80 is the lock location. In x16 Read-Electronic-
         * Signature mode the byte is presented on the lower byte only; factory lock
         * bit0 is programmed to 0, user-lock bit1 remains 1 until explicitly locked. */
        if (off == 0x0100) return FLASH_PR_LOCK_BYTE;
        /* Table 7: words 0x81..0x84 are the factory-programmed 64-bit UID.
         * Words 0x85..0x88 at 0x010a..0x0111 are a separate user-programmable
         * segment and must not alias this identity. */
        if (st->factory_uid_set && off >= 0x0102 && off <= 0x0109) {
            uint32_t k = (off - 0x0102) & ~1u;
            return st->factory_uid[k] | ((uint16_t)st->factory_uid[k + 1u] << 8);
        }
        return 0x0000;
    }
    return 0x0000;   /* cfi / other: not modeled */
}

static void flash_write_command(m58lw064d_state_t *st, uint32_t off,
                                uint8_t cmd) {
    st->cmd_writes++;
    switch (cmd) {
        case 0xFF:
            st->read_mode = FLASH_ARRAY;
            flash_command_reset(st);
            break;
        case 0x90:
            st->read_mode = FLASH_ID;
            flash_command_reset(st);
            break;
        case 0x70:
            st->read_mode = FLASH_STATUS;
            flash_command_reset(st);
            break;
        case 0x98:
            st->read_mode = FLASH_CFI;
            flash_command_reset(st);
            break;
        case 0x50:
            st->status = FLASH_STATUS_READY;
            flash_command_reset(st);
            break;
        case 0x40:
        case 0x10:
            st->command_phase = FLASH_PHASE_PROGRAM_DATA;
            flash_write_buffer_reset(st);
            break;
        case 0x20:
            st->command_phase = FLASH_PHASE_ERASE_CONFIRM;
            flash_write_buffer_reset(st);
            break;
        case 0xE8:
            /* Datasheet p.15: after the setup cycle, reads output status and the
             * following N+2 writes are buffer-count, payload words, and confirm. */
            st->read_mode = FLASH_STATUS;
            st->write_buffer_state = FLASH_WB_EXPECT_COUNT;
            st->write_buffer_words_left = 0;
            st->write_buffer_block =
                off & ~(M58LW064D_BLOCK_SIZE - 1u);
            st->write_buffer_page = FLASH_INVALID_PAGE;
            st->write_buffer_len = 0;
            break;
        default:
            break;   /* Unsupported protection/STS/suspend commands are absorbed. */
    }
}

static void flash_sequence_error(m58lw064d_state_t *st) {
    st->status |= FLASH_STATUS_SEQUENCE_ERROR;
    st->read_mode = FLASH_STATUS;
    flash_command_reset(st);
}

static void flash_access_reset(m58lw064d_access_t *access, const char *detail) {
    if (!access) return;
    access->subtype = NULL;
    access->detail = detail;
    access->include_mode = 0;
    access->mutation_count = 0;
}

uint8_t cemu_m58lw064d_read8(const uint8_t *data, size_t len,
                        m58lw064d_state_t *st, uint32_t off,
                        int command_visible, m58lw064d_access_t *access) {
    flash_access_reset(access, "flash");
    if (command_visible && st->read_mode != FLASH_ARRAY) {
        uint16_t word = flash_signature_word(st, off);
        if (access) access->detail = flash_read_detail(st, off);
        if (access) access->subtype = flash_read_subtype(st);
        return (off & 1) ? (uint8_t)((word >> 8) & 0xFF) : (uint8_t)(word & 0xFF);
    }
    return cemu_m58lw064d_array_read8(data, len, st, off);
}

static void flash_bus_write(const uint8_t *data, size_t len,
                            m58lw064d_state_t *st, uint32_t off,
                            uint16_t value, int size, int command_visible,
                            m58lw064d_access_t *access) {
    uint8_t cmd = (uint8_t)(value & 0xFF);
    flash_access_reset(access, "not-modeled");
    if (access) access->include_mode = 1;
    if (!command_visible) {
        if (access) access->subtype = flash_absorbed_write_subtype();
        return;
    }

    if (st->command_phase == FLASH_PHASE_PROGRAM_DATA) {
        int program_error = 0;
        int ok = flash_program_value(data, len, st, off, value, size,
                                     &program_error);
        st->command_phase = FLASH_PHASE_IDLE;
        st->read_mode = FLASH_STATUS;
        st->status |= !ok ? FLASH_STATUS_SEQUENCE_ERROR
                          : program_error
                            ? FLASH_STATUS_READY | FLASH_STATUS_PROGRAM_ERROR
                            : FLASH_STATUS_READY;
        if (access) {
            access->subtype = flash_command_subtype();
            access->detail = "program-data";
            if (ok)
                flash_access_add_mutation(
                    access, FLASH_MUTATION_PROGRAM, off, (uint32_t)size);
        }
        return;
    }

    if (st->command_phase == FLASH_PHASE_ERASE_CONFIRM) {
        st->cmd_writes++;
        if (cmd == 0xD0 && flash_erase_block(len, st, off)) {
            st->command_phase = FLASH_PHASE_IDLE;
            st->read_mode = FLASH_STATUS;
            st->status |= FLASH_STATUS_READY;
            if (access) {
                access->subtype = flash_command_subtype();
                access->detail = "block-erase-confirm";
                flash_access_add_mutation(
                    access, FLASH_MUTATION_ERASE,
                    off & ~(M58LW064D_BLOCK_SIZE - 1u),
                    M58LW064D_BLOCK_SIZE);
            }
        } else {
            flash_sequence_error(st);
            if (access) {
                access->subtype = flash_command_subtype();
                access->detail = "command-sequence-error";
            }
        }
        return;
    }

    if (st->write_buffer_state == FLASH_WB_EXPECT_COUNT) {
        uint32_t block = off & ~(M58LW064D_BLOCK_SIZE - 1u);
        if (block != st->write_buffer_block ||
                cmd >= M58LW064D_WRITE_BUFFER_WORDS) {
            flash_sequence_error(st);
            if (access) {
                access->subtype = flash_command_subtype();
                access->detail = "command-sequence-error";
            }
            return;
        }
        st->write_buffer_words_left = (uint8_t)(cmd + 1u);
        st->write_buffer_state = FLASH_WB_LOADING;
        if (access) {
            access->subtype = flash_command_subtype();
            access->detail = "write-buffer-count";
        }
        return;
    }
    if (st->write_buffer_state == FLASH_WB_LOADING) {
        uint32_t block = off & ~(M58LW064D_BLOCK_SIZE - 1u);
        uint32_t page = off & ~(M58LW064D_WRITE_BUFFER_SIZE - 1u);
        int valid = size == 2 && !(off & 1u) && off + 1u < len &&
                    block == st->write_buffer_block &&
                    (st->write_buffer_page == FLASH_INVALID_PAGE ||
                     page == st->write_buffer_page);
        if (!valid || !flash_write_buffer_push(st, off, value, size)) {
            flash_sequence_error(st);
            if (access) {
                access->subtype = flash_command_subtype();
                access->detail = "command-sequence-error";
            }
            return;
        }
        if (st->write_buffer_page == FLASH_INVALID_PAGE)
            st->write_buffer_page = page;
        if (st->write_buffer_words_left) st->write_buffer_words_left--;
        if (!st->write_buffer_words_left) st->write_buffer_state = FLASH_WB_EXPECT_CONFIRM;
        if (access) {
            access->subtype = flash_command_subtype();
            access->detail = "write-buffer-data";
        }
        return;
    }
    if (st->write_buffer_state == FLASH_WB_EXPECT_CONFIRM && cmd == 0xD0) {
        st->cmd_writes++;
        int program_error = 0;
        int ok = flash_commit_write_buffer(data, len, st, &program_error);
        st->read_mode = FLASH_STATUS;
        st->status |= !ok ? FLASH_STATUS_SEQUENCE_ERROR
                          : program_error
                            ? FLASH_STATUS_READY | FLASH_STATUS_PROGRAM_ERROR
                            : FLASH_STATUS_READY;
        if (access) {
            access->subtype = flash_command_subtype();
            access->detail = flash_cmd_name(cmd);
            if (ok) {
                for (size_t i = 0; i < st->write_buffer_len; i++) {
                    const m58lw064d_write_buffer_entry_t *entry =
                        &st->write_buffer[i];
                    flash_access_add_mutation(
                        access, FLASH_MUTATION_PROGRAM, entry->off,
                        entry->size);
                }
            }
        }
        flash_write_buffer_reset(st);
        return;
    }
    if (st->write_buffer_state == FLASH_WB_EXPECT_CONFIRM) {
        flash_sequence_error(st);
        if (access) {
            access->subtype = flash_command_subtype();
            access->detail = "command-sequence-error";
        }
        return;
    }

    flash_write_command(st, off, cmd);
    if (access) {
        access->subtype = flash_command_subtype();
        access->detail = flash_cmd_name(cmd);
    }
}

void cemu_m58lw064d_write8(const uint8_t *data, size_t len,
                      m58lw064d_state_t *st, uint32_t off, uint8_t value,
                      int command_visible, m58lw064d_access_t *access) {
    flash_bus_write(data, len, st, off, value, 1, command_visible, access);
}

void cemu_m58lw064d_write16(const uint8_t *data, size_t len,
                       m58lw064d_state_t *st, uint32_t off, uint16_t value,
                       int command_visible, m58lw064d_access_t *access) {
    flash_bus_write(data, len, st, off, value, 2, command_visible, access);
}

void cemu_m58lw064d_state_free(m58lw064d_state_t *st) {
    if (!st) return;
    free(st->write_buffer);
    cemu_nor_store_free(&st->store);
    memset(st, 0, sizeof(*st));
}

int cemu_m58lw064d_state_copy(m58lw064d_state_t *dst, const m58lw064d_state_t *src) {
    int ok = cemu_m58lw064d_state_restore(dst,
                                 src ? src->read_mode : FLASH_ARRAY,
                                 src ? src->command_phase : FLASH_PHASE_IDLE,
                                 src ? src->status : FLASH_STATUS_READY,
                                 src ? src->cmd_writes : 0,
                                 src ? src->erased_blocks : 0,
                                 src ? src->write_buffer_state : FLASH_WB_IDLE,
                                 src ? src->write_buffer_words_left : 0,
                                 src ? src->write_buffer_block : 0,
                                 src ? src->write_buffer_page : FLASH_INVALID_PAGE,
                                 src ? src->write_buffer : NULL,
                                 src ? src->write_buffer_len : 0,
                                 src ? src->store.patches : NULL,
                                 src ? src->store.count : 0);
    if (!ok) return 0;
    if (src && src->factory_uid_set) {
        cemu_m58lw064d_factory_uid_set(dst, src->factory_uid);
    } else {
        memset(dst->factory_uid, 0, sizeof(dst->factory_uid));
        dst->factory_uid_set = 0;
    }
    return 1;
}

static int flash_state_is_valid(
                        m58lw064d_mode_t read_mode,
                        m58lw064d_command_phase_t command_phase,
                        uint8_t status,
                        m58lw064d_write_buffer_state_t write_buffer_state,
                        uint8_t write_buffer_words_left,
                        uint32_t write_buffer_block,
                        uint32_t write_buffer_page,
                        const m58lw064d_write_buffer_entry_t *write_buffer,
                        size_t write_buffer_len,
                        const m58lw064d_patch_t *patches,
                        size_t patch_count) {
    if (read_mode < FLASH_ARRAY || read_mode > FLASH_CFI ||
            command_phase < FLASH_PHASE_IDLE ||
            command_phase > FLASH_PHASE_ERASE_CONFIRM ||
            write_buffer_state < FLASH_WB_IDLE ||
            write_buffer_state > FLASH_WB_EXPECT_CONFIRM)
        return 0;
    if (!(status & FLASH_STATUS_READY) ||
            (status & ~(FLASH_STATUS_READY | FLASH_STATUS_ERASE_ERROR |
                        FLASH_STATUS_PROGRAM_ERROR)))
        return 0;
    if ((write_buffer_len && !write_buffer) || (patch_count && !patches) ||
            write_buffer_len > M58LW064D_WRITE_BUFFER_WORDS ||
            patch_count > FLASH_CHIP_SIZE)
        return 0;
    if (command_phase != FLASH_PHASE_IDLE &&
            write_buffer_state != FLASH_WB_IDLE)
        return 0;

    if (!cemu_nor_store_validate(patches, patch_count, FLASH_CHIP_SIZE)) return 0;

    if (write_buffer_state == FLASH_WB_IDLE) {
        if (write_buffer_len || write_buffer_words_left ||
                write_buffer_block != 0 ||
                write_buffer_page != FLASH_INVALID_PAGE)
            return 0;
        return 1;
    }

    if (command_phase != FLASH_PHASE_IDLE ||
            write_buffer_block >= FLASH_CHIP_SIZE ||
            (write_buffer_block & (M58LW064D_BLOCK_SIZE - 1u)))
        return 0;
    if (write_buffer_state == FLASH_WB_EXPECT_COUNT) {
        return write_buffer_len == 0 && write_buffer_words_left == 0 &&
               write_buffer_page == FLASH_INVALID_PAGE;
    }
    if (write_buffer_len + write_buffer_words_left == 0 ||
            write_buffer_len + write_buffer_words_left >
                M58LW064D_WRITE_BUFFER_WORDS)
        return 0;
    if (write_buffer_state == FLASH_WB_LOADING &&
            write_buffer_words_left == 0)
        return 0;
    if (write_buffer_state == FLASH_WB_EXPECT_CONFIRM &&
            write_buffer_words_left != 0)
        return 0;
    if ((write_buffer_len == 0) !=
            (write_buffer_page == FLASH_INVALID_PAGE))
        return 0;
    if (write_buffer_page != FLASH_INVALID_PAGE &&
            (write_buffer_page >= FLASH_CHIP_SIZE ||
             (write_buffer_page & (M58LW064D_WRITE_BUFFER_SIZE - 1u)) ||
             write_buffer_page < write_buffer_block ||
             write_buffer_page >= write_buffer_block + M58LW064D_BLOCK_SIZE))
        return 0;
    for (size_t i = 0; i < write_buffer_len; i++) {
        const m58lw064d_write_buffer_entry_t *entry = &write_buffer[i];
        if (entry->size != 2 || (entry->off & 1u) ||
                entry->off + 1u >= FLASH_CHIP_SIZE ||
                (entry->off & ~(M58LW064D_BLOCK_SIZE - 1u)) !=
                    write_buffer_block ||
                (entry->off & ~(M58LW064D_WRITE_BUFFER_SIZE - 1u)) !=
                    write_buffer_page)
            return 0;
    }
    return 1;
}

int cemu_m58lw064d_state_restore(m58lw064d_state_t *st,
                        m58lw064d_mode_t read_mode,
                        m58lw064d_command_phase_t command_phase,
                        uint8_t status, uint32_t cmd_writes,
                        uint64_t erased_blocks,
                        m58lw064d_write_buffer_state_t write_buffer_state,
                        uint8_t write_buffer_words_left,
                        uint32_t write_buffer_block,
                        uint32_t write_buffer_page,
                        const m58lw064d_write_buffer_entry_t *write_buffer,
                        size_t write_buffer_len,
                        const m58lw064d_patch_t *patches,
                        size_t patch_count) {
    if (!st || !flash_state_is_valid(
            read_mode, command_phase, status, write_buffer_state,
            write_buffer_words_left, write_buffer_block, write_buffer_page,
            write_buffer, write_buffer_len, patches, patch_count))
        return 0;
    m58lw064d_state_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.read_mode = read_mode;
    tmp.command_phase = command_phase;
    tmp.status = status;
    /* Runtime restores must not erase explicitly configured physical chip identity. */
    if (st && st->factory_uid_set) {
        memcpy(tmp.factory_uid, st->factory_uid, sizeof(tmp.factory_uid));
        tmp.factory_uid_set = 1;
    }
    tmp.cmd_writes = cmd_writes;
    tmp.erased_blocks = erased_blocks;
    tmp.write_buffer_state = write_buffer_state;
    tmp.write_buffer_words_left = write_buffer_words_left;
    tmp.write_buffer_block = write_buffer_block;
    tmp.write_buffer_page = write_buffer_page;
    if (write_buffer_len) {
        tmp.write_buffer = malloc(write_buffer_len * sizeof(*tmp.write_buffer));
        if (!tmp.write_buffer) return 0;
        memcpy(tmp.write_buffer, write_buffer, write_buffer_len * sizeof(*tmp.write_buffer));
        tmp.write_buffer_len = write_buffer_len;
        tmp.write_buffer_cap = write_buffer_len;
    }
    if (!cemu_nor_store_restore(&tmp.store, patches, patch_count,
                           FLASH_CHIP_SIZE)) {
        free(tmp.write_buffer);
        return 0;
    }
    cemu_m58lw064d_state_free(st);
    *st = tmp;
    return 1;
}

const m58lw064d_patch_t *cemu_m58lw064d_state_patches(const m58lw064d_state_t *st, size_t *count_out) {
    return st ? cemu_nor_store_patches(&st->store, count_out) : NULL;
}

const m58lw064d_write_buffer_entry_t *cemu_m58lw064d_state_write_buffer_entries(const m58lw064d_state_t *st,
                                                                   size_t *count_out) {
    if (count_out) *count_out = st ? st->write_buffer_len : 0;
    return st ? st->write_buffer : NULL;
}

void cemu_m58lw064d_periph_init(peripheral_t *p, m58lw064d_state_t *st) {
    memset(p, 0, sizeof(*p));
    memset(st, 0, sizeof(*st));
    st->read_mode = FLASH_ARRAY;
    st->command_phase = FLASH_PHASE_IDLE;
    st->status = FLASH_STATUS_READY;
    st->write_buffer_page = FLASH_INVALID_PAGE;
    p->id = "flash";
    p->model = "m58lw064d";
    p->state = st;
    /* Flash claims no SFR words or byte ranges — the SoC only decides which
     * mapped flash view was hit, then delegates the access semantics here. */
    p->sfr_words = NULL;    p->n_sfr_words = 0;
    p->byte_ranges = NULL;  p->n_byte_ranges = 0;
    p->ic_nodes = NULL;     p->n_ic_nodes = 0;
    p->reg_names = NULL;    p->n_reg_names = 0;
    p->read8 = NULL;   p->peek8 = NULL;   p->write8 = NULL;
    p->read_sfr_word = NULL; p->on_sfr_poll = NULL; p->on_sfr_write = NULL;
    p->tick = NULL;    p->timer_running = NULL;
}
