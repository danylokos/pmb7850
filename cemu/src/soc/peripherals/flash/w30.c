#include "w30.h"

#include <string.h>

#define W30_SR_READY           0x80u
#define W30_SR_ERASE_SUSPEND   0x40u
#define W30_SR_ERASE_ERROR     0x20u
#define W30_SR_PROGRAM_ERROR   0x10u
#define W30_SR_VPP_ERROR       0x08u
#define W30_SR_PROGRAM_SUSPEND 0x04u
#define W30_SR_LOCK_ERROR      0x02u
#define W30_SR_OTHER_BUSY      0x01u
#define W30_SR_CLEARABLE \
    (W30_SR_ERASE_ERROR | W30_SR_PROGRAM_ERROR | W30_SR_VPP_ERROR | \
     W30_SR_LOCK_ERROR)

const w30_config_t cemu_W30_64MBIT_TOP = {
    .model = W30_64MBIT_TOP_MODEL,
    .size = W30_64MBIT_TOP_SIZE,
    .manufacturer_id = W30_MANUFACTURER_ID,
    .device_id = W30_64MBIT_TOP_DEVICE_ID,
    .cfi_device_size = W30_64MBIT_TOP_CFI_DEVICE_SIZE,
    .cfi_erase_region_count = W30_64MBIT_TOP_CFI_ERASE_REGIONS,
    .partition_count = W30_64MBIT_TOP_PARTITION_COUNT,
    .main_block_count = W30_64MBIT_TOP_MAIN_BLOCK_COUNT,
    .parameter_base = W30_64MBIT_TOP_PARAMETER_BASE,
    .block_count = W30_64MBIT_TOP_BLOCK_COUNT,
};

const w30_config_t cemu_W30_128MBIT_TOP = {
    .model = W30_128MBIT_TOP_MODEL,
    .size = W30_128MBIT_TOP_SIZE,
    .manufacturer_id = W30_MANUFACTURER_ID,
    .device_id = W30_128MBIT_TOP_DEVICE_ID,
    .cfi_device_size = W30_128MBIT_TOP_CFI_DEVICE_SIZE,
    .cfi_erase_region_count = W30_128MBIT_TOP_CFI_ERASE_REGIONS,
    .partition_count = W30_128MBIT_TOP_PARTITION_COUNT,
    .main_block_count = W30_128MBIT_TOP_MAIN_BLOCK_COUNT,
    .parameter_base = W30_128MBIT_TOP_PARAMETER_BASE,
    .block_count = W30_128MBIT_TOP_BLOCK_COUNT,
};

static uint8_t partition_for(const w30_state_t *st,
                             uint32_t off) {
    uint32_t partition_size = st->config->size / st->config->partition_count;
    return (uint8_t)(off / partition_size);
}

static int bit_test(const uint64_t bits[W30_MAX_BLOCK_BITMAP_WORDS],
                    unsigned index) {
    return (bits[index >> 6] & (UINT64_C(1) << (index & 63u))) != 0;
}

static void bit_set(uint64_t bits[W30_MAX_BLOCK_BITMAP_WORDS],
                    unsigned index, int value) {
    uint64_t mask = UINT64_C(1) << (index & 63u);
    if (value) bits[index >> 6] |= mask;
    else bits[index >> 6] &= ~mask;
}

static int block_for_config(const w30_config_t *config, uint32_t off,
                            uint32_t *start, uint32_t *size) {
    if (!config || off >= config->size) return -1;
    unsigned block;
    uint32_t block_start, block_size;
    if (off < config->parameter_base) {
        block = off / W30_MAIN_BLOCK_SIZE;
        block_start = block * W30_MAIN_BLOCK_SIZE;
        block_size = W30_MAIN_BLOCK_SIZE;
    } else {
        block = config->main_block_count +
                (off - config->parameter_base) / W30_PARAMETER_BLOCK_SIZE;
        block_start = config->parameter_base +
            (block - config->main_block_count) * W30_PARAMETER_BLOCK_SIZE;
        block_size = W30_PARAMETER_BLOCK_SIZE;
    }
    if (start) *start = block_start;
    if (size) *size = block_size;
    return (int)block;
}

int cemu_w30_block_for_offset(const w30_state_t *st, uint32_t off,
                         uint32_t *start, uint32_t *size) {
    return block_for_config(st ? st->config : NULL, off, start, size);
}

unsigned cemu_w30_block_bitmap_words(const w30_state_t *st) {
    return st && st->config ? (st->config->block_count + 63u) / 64u : 0;
}

const char *cemu_w30_model_str(const w30_state_t *st) {
    return st && st->config ? st->config->model : "invalid";
}

static void access_reset(w30_access_t *access, const char *detail) {
    if (!access) return;
    access->subtype = NULL;
    access->detail = detail;
    access->include_mode = 0;
    access->mutation_count = 0;
}

const char *cemu_w30_operation_str(w30_operation_kind_t kind) {
    switch (kind) {
        case W30_OP_PROGRAM: return "program";
        case W30_OP_ERASE: return "erase";
        case W30_OP_PROTECTION_PROGRAM: return "protection-program";
        default: return "none";
    }
}

const char *cemu_w30_phase_str(w30_phase_t phase) {
    switch (phase) {
        case W30_PHASE_PROGRAM_DATA: return "program-data";
        case W30_PHASE_ERASE_CONFIRM: return "erase-confirm";
        case W30_PHASE_LOCK_CONFIRM: return "lock-confirm";
        case W30_PHASE_PROTECTION_DATA: return "protection-data";
        case W30_PHASE_EFP_CONFIRM: return "efp-confirm";
        default: return "idle";
    }
}

const char *cemu_w30_mode_str(const w30_state_t *st) {
    if (!st || !st->config) return "invalid";
    for (unsigned i = 0; i < st->config->partition_count; i++) {
        if (st->mode[i] == W30_MODE_ID) return "id";
        if (st->mode[i] == W30_MODE_CFI) return "cfi";
        if (st->mode[i] == W30_MODE_STATUS) return "status";
    }
    return "array";
}

static const char *mode_subtype(w30_mode_t mode) {
    if (mode == W30_MODE_ID) return "id";
    if (mode == W30_MODE_CFI) return "cfi";
    if (mode == W30_MODE_STATUS) return "status";
    return NULL;
}

int cemu_w30_seed_bytes(const uint8_t *data, size_t len,
                   w30_state_t *st, uint32_t off,
                   const uint8_t *bytes, size_t count) {
    return data && st && st->config && len <= st->config->size &&
           cemu_nor_store_seed(&st->store, (uint32_t)len, off, bytes, count);
}

uint8_t cemu_w30_array_read8(const uint8_t *data, size_t len,
                        const w30_state_t *st, uint32_t off) {
    int block = cemu_w30_block_for_offset(st, off, NULL, NULL);
    int erased = block >= 0 && bit_test(st->erased_blocks, (unsigned)block);
    return cemu_nor_store_read(&st->store, data, len, off, erased);
}

static uint8_t cfi_byte(const w30_config_t *config, unsigned word) {
    static const uint8_t cfi[0x78] = {
        [0x10] = 'Q', [0x11] = 'R', [0x12] = 'Y',
        [0x13] = 0x03, [0x15] = 0x39,
        [0x1B] = 0x17, [0x1C] = 0x19, [0x1D] = 0xB4, [0x1E] = 0xC6,
        [0x1F] = 0x04, [0x21] = 0x0A, [0x23] = 0x04,
        [0x25] = 0x03, [0x27] = 0x17, [0x28] = 0x01,
        [0x2C] = 0x11,
        [0x2D] = 0x07, [0x30] = 0x01,
        [0x31] = 0x06, [0x34] = 0x01,
        [0x35] = 0x07, [0x37] = 0x20,
        [0x39] = 'P', [0x3A] = 'R', [0x3B] = 'I',
        [0x3C] = '1', [0x3D] = '3',
        [0x3E] = 0xE6, [0x3F] = 0x03,
        [0x42] = 0x01, [0x43] = 0x03, [0x45] = 0x18, [0x46] = 0xC0,
        [0x47] = 0x01, [0x48] = 0x80, [0x4A] = 0x03, [0x4B] = 0x03,
        [0x4C] = 0x03, [0x4D] = 0x03, [0x4E] = 0x01,
        [0x4F] = 0x02, [0x50] = 0x07,
        [0x51] = 0x02, [0x52] = 0x0F,
        [0x54] = 0x01, [0x57] = 0x0F,
        [0x58] = 0x07, [0x5B] = 0x01,
        [0x5C] = 0x64, [0x5E] = 0x01, [0x5F] = 0x03,
        [0x60] = 0x01, [0x62] = 0x01, [0x65] = 0x02,
        [0x66] = 0x06, [0x69] = 0x01,
        [0x6A] = 0x64, [0x6C] = 0x01, [0x6D] = 0x03,
        [0x6E] = 0x07, [0x70] = 0x20,
        [0x72] = 0x64, [0x74] = 0x01, [0x75] = 0x02,
    };
    if (!config) return 0;
    if (word == 0x27) return config->cfi_device_size;
    if (word == 0x2C) return config->cfi_erase_region_count;
    if (word == 0x52 || word == 0x57)
        return (uint8_t)(config->partition_count - 1u);
    return word < sizeof cfi ? cfi[word] : 0;
}

static uint16_t id_word(const w30_state_t *st, uint32_t off) {
    off &= ~1u;
    uint32_t partition_size =
        st->config->size / st->config->partition_count;
    uint32_t partition_base = off - off % partition_size;
    uint32_t relative = off - partition_base;
    uint32_t id_relative = relative & 0x1FFu;
    uint32_t block_start = 0;
    int block = cemu_w30_block_for_offset(st, off, &block_start, NULL);
    uint32_t block_relative = off - block_start;
    if (block_relative == 0) return st->config->manufacturer_id;
    if (block_relative == 2) return st->config->device_id;
    if (block_relative == 4u && block >= 0)
        return bit_test(st->locked_blocks, (unsigned)block) |
               (bit_test(st->lockdown_blocks, (unsigned)block) << 1);
    if (id_relative == 0x0A)
        return st->configuration[partition_for(st, off)];
    if (id_relative == 0x100)
        return st->customer_locked ? 0xFFFC : 0xFFFE;
    if (id_relative >= 0x102 && id_relative <= 0x109 &&
        st->factory_uid_set) {
        unsigned i = id_relative - 0x102;
        i &= ~1u;
        return st->factory_uid[i] | ((uint16_t)st->factory_uid[i + 1u] << 8);
    }
    if (id_relative >= 0x10A && id_relative <= 0x111) {
        unsigned i = id_relative - 0x10A;
        i &= ~1u;
        return st->customer[i] | ((uint16_t)st->customer[i + 1u] << 8);
    }
    return 0;
}

static uint8_t status_value(const w30_state_t *st,
                            unsigned partition) {
    uint8_t value = st->status[partition] & W30_SR_CLEARABLE;
    if (st->active.kind != W30_OP_NONE) {
        if (partition == st->active.partition) {
            value &= ~(W30_SR_READY | W30_SR_OTHER_BUSY);
        } else {
            value &= ~W30_SR_READY;
            value |= W30_SR_OTHER_BUSY;
        }
    } else {
        value |= W30_SR_READY;
    }
    if (st->suspended_erase.kind == W30_OP_ERASE)
        value |= W30_SR_ERASE_SUSPEND;
    if (st->suspended_program.kind == W30_OP_PROGRAM)
        value |= W30_SR_PROGRAM_SUSPEND;
    return value;
}

static uint8_t read_impl(const uint8_t *data, size_t len,
                         w30_state_t *st, uint32_t off,
                         int command_visible, w30_access_t *access) {
    access_reset(access, "flash");
    if (!st || !st->config || !command_visible || off >= st->config->size)
        return cemu_w30_array_read8(data, len, st, off);
    unsigned partition = partition_for(st, off);
    w30_mode_t mode = st->mode[partition];
    if (st->active.kind != W30_OP_NONE &&
        partition == st->active.partition && mode != W30_MODE_CFI)
        mode = W30_MODE_STATUS;
    if (mode == W30_MODE_ARRAY)
        return cemu_w30_array_read8(data, len, st, off);
    uint16_t value = 0;
    if (mode == W30_MODE_ID) value = id_word(st, off);
    else if (mode == W30_MODE_CFI) {
        uint32_t partition_size =
            st->config->size / st->config->partition_count;
        uint32_t word = (off % partition_size) >> 1;
        value = cfi_byte(st->config, word);
    } else value = status_value(st, partition);
    if (access) {
        access->subtype = mode_subtype(mode);
        access->detail = mode == W30_MODE_STATUS ? "status-register" :
                         mode == W30_MODE_CFI ? "cfi" : "identity";
        access->include_mode = 1;
    }
    return (off & 1u) ? (uint8_t)(value >> 8) : (uint8_t)value;
}

uint8_t cemu_w30_read8(const uint8_t *data, size_t len,
                  w30_state_t *st, uint32_t off,
                  int command_visible, w30_access_t *access) {
    return read_impl(data, len, st, off, command_visible, access);
}

uint8_t cemu_w30_peek8(const uint8_t *data, size_t len,
                  const w30_state_t *st, uint32_t off,
                  int command_visible) {
    return read_impl(data, len, (w30_state_t *)st, off,
                     command_visible, NULL);
}

static void sequence_error(w30_state_t *st, unsigned partition) {
    st->status[partition] |= W30_SR_PROGRAM_ERROR | W30_SR_ERASE_ERROR;
    st->mode[partition] = W30_MODE_STATUS;
    st->phase = W30_PHASE_IDLE;
}

static void start_program(const uint8_t *data, size_t len,
                          w30_state_t *st, uint32_t off,
                          uint16_t requested, int size,
                          w30_access_t *access) {
    unsigned partition = partition_for(st, off);
    int block = cemu_w30_block_for_offset(st, off, NULL, NULL);
    if (size != 2 || (off & 1u) || off + 1u >= len || block < 0) {
        sequence_error(st, partition);
        if (access) access->detail = "command-sequence-error";
        return;
    }
    if (bit_test(st->locked_blocks, (unsigned)block)) {
        st->status[partition] |= W30_SR_LOCK_ERROR | W30_SR_PROGRAM_ERROR;
        st->mode[partition] = W30_MODE_STATUS;
        if (access) access->detail = "program-locked";
        return;
    }
    if (st->suspended_erase.kind == W30_OP_ERASE &&
        st->suspended_erase.block == (unsigned)block) {
        sequence_error(st, partition);
        if (access) access->detail = "program-in-suspended-erase-block";
        return;
    }
    uint16_t old = cemu_w30_array_read8(data, len, st, off) |
        ((uint16_t)cemu_w30_array_read8(data, len, st, off + 1u) << 8);
    st->active = (w30_operation_t){
        .kind = W30_OP_PROGRAM, .partition = partition,
        .block = (uint16_t)block, .off = off, .value = old & requested,
        .ticks_remaining = W30_PROGRAM_TICKS,
        .program_error = (old & requested) != requested,
    };
    st->mode[partition] = W30_MODE_STATUS;
    if (access) {
        access->subtype = "program";
        access->detail = "word-program-start";
    }
}

static void start_erase(w30_state_t *st, uint32_t off,
                        w30_access_t *access) {
    unsigned partition = partition_for(st, off);
    int block = cemu_w30_block_for_offset(st, off, NULL, NULL);
    if (block < 0 || st->suspended_erase.kind != W30_OP_NONE) {
        sequence_error(st, partition);
        return;
    }
    if (bit_test(st->locked_blocks, (unsigned)block)) {
        st->status[partition] |= W30_SR_LOCK_ERROR | W30_SR_ERASE_ERROR;
        st->mode[partition] = W30_MODE_STATUS;
        if (access) access->detail = "erase-locked";
        return;
    }
    st->active = (w30_operation_t){
        .kind = W30_OP_ERASE, .partition = partition,
        .block = (uint16_t)block, .off = off,
        .ticks_remaining = W30_ERASE_TICKS,
    };
    st->mode[partition] = W30_MODE_STATUS;
    if (access) {
        access->subtype = "cmd";
        access->detail = "block-erase-confirm";
    }
}

static void protection_program(w30_state_t *st, uint32_t off,
                               uint16_t value, int size,
                               w30_access_t *access) {
    unsigned partition = partition_for(st, off);
    uint32_t partition_size =
        st->config->size / st->config->partition_count;
    uint32_t relative = off % partition_size;
    if (partition != 0 || size != 2 || (off & 1u) || relative < 0x100 ||
        relative > 0x111) {
        st->status[partition] |= W30_SR_PROGRAM_ERROR;
        st->mode[partition] = W30_MODE_STATUS;
        st->phase = W30_PHASE_IDLE;
        if (access) access->detail = "protection-address-invalid";
        return;
    }
    if (relative == 0x100) {
        if (st->customer_locked) {
            st->status[partition] |= W30_SR_LOCK_ERROR | W30_SR_PROGRAM_ERROR;
            st->mode[partition] = W30_MODE_STATUS;
        } else {
            st->active = (w30_operation_t){
                .kind = W30_OP_PROTECTION_PROGRAM,
                .partition = partition, .off = off, .value = value,
                .ticks_remaining = W30_PROGRAM_TICKS,
            };
            st->mode[partition] = W30_MODE_STATUS;
        }
    } else if (relative >= 0x10A && !st->customer_locked) {
        unsigned i = relative - 0x10A;
        st->active = (w30_operation_t){
            .kind = W30_OP_PROTECTION_PROGRAM,
            .partition = partition, .off = off,
            .value = (uint16_t)(st->customer[i] & (uint8_t)value) |
                ((uint16_t)(st->customer[i + 1u] &
                            (uint8_t)(value >> 8)) << 8),
            .ticks_remaining = W30_PROGRAM_TICKS,
            .program_error =
                ((st->customer[i] | st->customer[i + 1u] << 8) & value) !=
                    value,
        };
        st->mode[partition] = W30_MODE_STATUS;
    } else if (relative < 0x10A) {
        st->status[partition] |= W30_SR_LOCK_ERROR | W30_SR_PROGRAM_ERROR;
        st->mode[partition] = W30_MODE_STATUS;
    }
    if (access) {
        access->subtype = "program";
        access->detail = relative == 0x100 ? "protection-lock" :
                         st->customer_locked ? "protection-locked" :
                         "protection-program-start";
    }
}

static void bus_write(const uint8_t *data, size_t len,
                      w30_state_t *st, uint32_t off,
                      uint16_t value, int size, int command_visible,
                      w30_access_t *access) {
    uint8_t cmd = value & 0xFFu;
    unsigned partition =
        st && st->config && off < st->config->size ? partition_for(st, off) : 0;
    access_reset(access, "rejected-command");
    if (access) access->include_mode = 1;
    if (!st || !st->config || !command_visible || off >= st->config->size) {
        if (access) access->subtype = "write-absorbed";
        return;
    }

    int read_while_active =
        st->active.kind != W30_OP_NONE &&
        ((partition != st->active.partition &&
          (cmd == 0xFF || cmd == 0x90 || cmd == 0x98 ||
           cmd == 0x70 || cmd == 0x50)) ||
         (partition == st->active.partition &&
          (cmd == 0x98 || cmd == 0x70)));
    if (st->active.kind != W30_OP_NONE && !read_while_active) {
        if (cmd == 0xB0) {
            int active_block =
                cemu_w30_block_for_offset(st, off, NULL, NULL);
            if (st->active.kind == W30_OP_ERASE &&
                st->suspended_erase.kind == W30_OP_NONE &&
                active_block == st->active.block) {
                st->suspended_erase = st->active;
                st->active = (w30_operation_t){0};
                st->mode[st->suspended_erase.partition] = W30_MODE_STATUS;
                if (access) {
                    access->subtype = "cmd";
                    access->detail = "erase-suspend";
                }
            } else if (st->active.kind == W30_OP_PROGRAM &&
                       st->suspended_program.kind == W30_OP_NONE &&
                       active_block == st->active.block) {
                st->suspended_program = st->active;
                st->active = (w30_operation_t){0};
                st->mode[st->suspended_program.partition] = W30_MODE_STATUS;
                if (access) {
                    access->subtype = "cmd";
                    access->detail = "program-suspend";
                }
            } else {
                sequence_error(st, partition);
                if (access) access->subtype = "write-absorbed";
            }
        } else if (access) {
            access->subtype = "write-absorbed";
            access->detail = "operation-busy";
        }
        return;
    }

    if (st->suspended_program.kind == W30_OP_PROGRAM &&
        cmd != 0xD0 && cmd != 0xFF && cmd != 0x90 && cmd != 0x98 &&
        cmd != 0x70 && cmd != 0x50) {
        sequence_error(st, partition);
        if (access) {
            access->subtype = "write-absorbed";
            access->detail = "command-sequence-error";
        }
        return;
    }

    if (st->phase == W30_PHASE_PROGRAM_DATA) {
        st->phase = W30_PHASE_IDLE;
        if (off != st->setup_off) {
            sequence_error(st, partition);
            if (access) access->detail = "command-sequence-error";
        } else {
            start_program(data, len, st, off, value, size, access);
        }
        return;
    }
    if (st->phase == W30_PHASE_ERASE_CONFIRM) {
        st->phase = W30_PHASE_IDLE;
        int block = cemu_w30_block_for_offset(st, off, NULL, NULL);
        int setup_block =
            cemu_w30_block_for_offset(st, st->setup_off, NULL, NULL);
        if (cmd == 0xD0 && block >= 0 && block == setup_block)
            start_erase(st, off, access);
        else {
            sequence_error(st, partition);
            if (access) {
                access->subtype = "write-absorbed";
                access->detail = "command-sequence-error";
            }
        }
        return;
    }
    if (st->phase == W30_PHASE_EFP_CONFIRM) {
        st->phase = W30_PHASE_IDLE;
        sequence_error(st, partition);
        if (access) {
            access->subtype = "write-absorbed";
            access->detail = "enhanced-factory-program-unsupported";
        }
        return;
    }
    if (st->phase == W30_PHASE_PROTECTION_DATA) {
        st->phase = W30_PHASE_IDLE;
        if (off != st->setup_off) {
            sequence_error(st, partition);
            if (access) access->detail = "command-sequence-error";
        } else {
            protection_program(st, off, value, size, access);
        }
        return;
    }
    if (st->phase == W30_PHASE_LOCK_CONFIRM) {
        st->phase = W30_PHASE_IDLE;
        int block = cemu_w30_block_for_offset(st, off, NULL, NULL);
        int setup_block =
            cemu_w30_block_for_offset(st, st->setup_off, NULL, NULL);
        if (block < 0 ||
            (cmd == 0x03 ? off != st->setup_off : block != setup_block)) {
            sequence_error(st, partition);
            return;
        }
        if (cmd == 0x01) bit_set(st->locked_blocks, block, 1);
        else if (cmd == 0xD0) {
            /* Both target schematics hold WP# high, so lock-down does not
             * prevent software unlock. */
            bit_set(st->locked_blocks, block, 0);
        } else if (cmd == 0x2F) {
            bit_set(st->locked_blocks, block, 1);
            bit_set(st->lockdown_blocks, block, 1);
        } else if (cmd == 0x03) {
            st->configuration[partition] = (uint16_t)(off >> 1);
            st->mode[partition] = W30_MODE_ARRAY;
        } else {
            sequence_error(st, partition);
            if (access) access->detail = "command-sequence-error";
            return;
        }
        if (access) {
            access->subtype = "cmd";
            access->detail = cmd == 0x01 ? "block-lock" :
                             cmd == 0xD0 ? "block-unlock" :
                             cmd == 0x2F ? "block-lock-down" :
                             "configuration-set";
        }
        return;
    }

    st->cmd_writes++;
    st->setup_partition = partition;
    st->setup_off = off;
    if (cmd == 0xFF) st->mode[partition] = W30_MODE_ARRAY;
    else if (cmd == 0x90) st->mode[partition] = W30_MODE_ID;
    else if (cmd == 0x98) st->mode[partition] = W30_MODE_CFI;
    else if (cmd == 0x70) st->mode[partition] = W30_MODE_STATUS;
    else if (cmd == 0x50) {
        st->status[partition] &= ~W30_SR_CLEARABLE;
        st->mode[partition] = W30_MODE_ARRAY;
    } else if (cmd == 0x40 || cmd == 0x10) {
        st->phase = W30_PHASE_PROGRAM_DATA;
    } else if (cmd == 0x20) {
        st->phase = W30_PHASE_ERASE_CONFIRM;
    } else if (cmd == 0x60) {
        st->phase = W30_PHASE_LOCK_CONFIRM;
    } else if (cmd == 0xC0) {
        st->phase = W30_PHASE_PROTECTION_DATA;
    } else if (cmd == 0x30) {
        st->phase = W30_PHASE_EFP_CONFIRM;
    } else if (cmd == 0xD0 &&
               (st->suspended_program.kind == W30_OP_PROGRAM ||
                st->suspended_erase.kind == W30_OP_ERASE)) {
        int program_resume =
            st->suspended_program.kind == W30_OP_PROGRAM;
        if (st->suspended_program.kind == W30_OP_PROGRAM) {
            st->active = st->suspended_program;
            st->suspended_program = (w30_operation_t){0};
        } else {
            st->active = st->suspended_erase;
            st->suspended_erase = (w30_operation_t){0};
        }
        if (access)
            access->detail = program_resume ? "program-resume" :
                                              "erase-resume";
    } else {
        if (access) access->subtype = "write-absorbed";
        return;
    }
    if (access) {
        access->subtype = "cmd";
        if (cmd == 0x20) access->detail = "block-erase-setup";
        else if (cmd == 0x40 || cmd == 0x10)
            access->detail = "word-program-setup";
        else if (cmd == 0x60) access->detail = "lock/configuration-setup";
        else if (cmd == 0xC0) access->detail = "protection-program-setup";
        else if (cmd == 0x30)
            access->detail = "enhanced-factory-program-setup";
        else if (cmd == 0xD0)
            access->detail = st->active.kind == W30_OP_PROGRAM
                           ? "program-resume" : "erase-resume";
        else if (cmd == 0xFF) access->detail = "read-array";
        else if (cmd == 0x90) access->detail = "read-id";
        else if (cmd == 0x98) access->detail = "read-query";
        else if (cmd == 0x70) access->detail = "read-status";
        else access->detail = "clear-status";
    }
}

void cemu_w30_write8(const uint8_t *data, size_t len,
                w30_state_t *st, uint32_t off,
                uint8_t value, int command_visible,
                w30_access_t *access) {
    bus_write(data, len, st, off, value, 1, command_visible, access);
}

void cemu_w30_write16(const uint8_t *data, size_t len,
                 w30_state_t *st, uint32_t off,
                 uint16_t value, int command_visible,
                 w30_access_t *access) {
    bus_write(data, len, st, off, value, 2, command_visible, access);
}

static uint32_t complete_operation(w30_state_t *st) {
    w30_operation_t op = st->active;
    st->active = (w30_operation_t){0};
    st->status[op.partition] |= op.program_error ? W30_SR_PROGRAM_ERROR : 0;
    if (op.kind == W30_OP_PROGRAM) {
        uint8_t bytes[2] = {(uint8_t)op.value, (uint8_t)(op.value >> 8)};
        (void)cemu_nor_store_seed(&st->store, st->config->size, op.off, bytes, 2);
        return W30_TICK_PROGRAM_COMPLETE;
    }
    if (op.kind == W30_OP_PROTECTION_PROGRAM) {
        uint32_t partition_size =
            st->config->size / st->config->partition_count;
        uint32_t relative = op.off % partition_size;
        if (relative == 0x100) {
            if ((op.value & 2u) == 0) st->customer_locked = 1;
        } else {
            unsigned i = relative - 0x10A;
            st->customer[i] = (uint8_t)op.value;
            st->customer[i + 1u] = (uint8_t)(op.value >> 8);
        }
        return W30_TICK_PROTECTION_COMPLETE;
    }
    uint32_t start = 0, size = 0;
    if (cemu_w30_block_for_offset(st, op.off, &start, &size) < 0)
        return W30_TICK_NO_EVENT;
    cemu_nor_store_erase_range(&st->store, start, start + size);
    bit_set(st->erased_blocks, op.block, 1);
    return W30_TICK_ERASE_COMPLETE;
}

uint32_t cemu_w30_tick(w30_state_t *st, uint32_t ticks) {
    if (!st || st->active.kind == W30_OP_NONE || !ticks)
        return W30_TICK_NO_EVENT;
    if (ticks < st->active.ticks_remaining) {
        st->active.ticks_remaining -= ticks;
        return W30_TICK_NO_EVENT;
    }
    return complete_operation(st);
}

uint64_t cemu_w30_next_event_ticks(const w30_state_t *st) {
    return st && st->active.kind != W30_OP_NONE
         ? st->active.ticks_remaining : UINT64_MAX;
}

void cemu_w30_advance_quiet(w30_state_t *st, uint64_t ticks) {
    if (!st || st->active.kind == W30_OP_NONE || !ticks) return;
    st->active.ticks_remaining -= (uint32_t)ticks;
}

void cemu_w30_factory_uid_set(w30_state_t *st,
                         const uint8_t uid[8]) {
    if (!st || !uid) return;
    memcpy(st->factory_uid, uid, sizeof st->factory_uid);
    st->factory_uid_set = 1;
}

void cemu_w30_reset(w30_state_t *st) {
    if (!st || !st->config) return;
    for (unsigned i = 0; i < st->config->partition_count; i++) {
        st->mode[i] = W30_MODE_ARRAY;
        st->status[i] = W30_SR_READY;
    }
    st->phase = W30_PHASE_IDLE;
    st->active = (w30_operation_t){0};
    st->suspended_erase = (w30_operation_t){0};
    st->suspended_program = (w30_operation_t){0};
    memset(st->locked_blocks, 0xFF, sizeof st->locked_blocks);
    memset(st->lockdown_blocks, 0, sizeof st->lockdown_blocks);
}

void cemu_w30_state_free(w30_state_t *st) {
    if (!st) return;
    cemu_nor_store_free(&st->store);
    memset(st, 0, sizeof(*st));
}

int cemu_w30_state_copy(w30_state_t *dst,
                   const w30_state_t *src) {
    if (!dst || !src || !src->config ||
        !cemu_nor_store_validate(src->store.patches, src->store.count,
                            src->config->size))
        return 0;
    w30_state_t tmp = *src;
    memset(&tmp.store, 0, sizeof tmp.store);
    if (!cemu_nor_store_copy(&tmp.store, &src->store, src->config->size)) return 0;
    cemu_w30_state_free(dst);
    *dst = tmp;
    return 1;
}

static int operation_valid(const w30_state_t *st,
                           const w30_operation_t *op, int suspended) {
    if (op->kind == W30_OP_NONE)
        return !op->partition && !op->block && !op->off && !op->value &&
               !op->ticks_remaining && !op->program_error;
    if (!st || !st->config ||
        op->partition >= st->config->partition_count ||
        op->block >= st->config->block_count ||
        op->off >= st->config->size ||
        !op->ticks_remaining)
        return 0;
    if (op->kind == W30_OP_PROTECTION_PROGRAM &&
        (op->partition != 0 ||
         (op->off != 0x100 &&
          (op->off < 0x10A || op->off > 0x110 || (op->off & 1u)))))
        return 0;
    if (op->kind == W30_OP_PROGRAM || op->kind == W30_OP_ERASE) {
        int block = cemu_w30_block_for_offset(st, op->off, NULL, NULL);
        if (block < 0 || op->block != (unsigned)block ||
            op->partition != partition_for(st, op->off))
            return 0;
    }
    if (op->kind == W30_OP_PROGRAM &&
        ((op->off & 1u) || op->ticks_remaining > W30_PROGRAM_TICKS))
        return 0;
    if (op->kind == W30_OP_ERASE &&
        op->ticks_remaining > W30_ERASE_TICKS)
        return 0;
    if (op->kind == W30_OP_PROTECTION_PROGRAM &&
        op->ticks_remaining > W30_PROGRAM_TICKS)
        return 0;
    if (suspended && op->kind != W30_OP_ERASE &&
        op->kind != W30_OP_PROGRAM)
        return 0;
    return op->kind >= W30_OP_PROGRAM &&
           op->kind <= W30_OP_PROTECTION_PROGRAM;
}

int cemu_w30_state_restore(w30_state_t *dst,
                      const w30_state_t *serialized,
                      const nor_patch_t *patches,
                      size_t patch_count) {
    if (!dst || !serialized || !serialized->config ||
        serialized->phase < W30_PHASE_IDLE ||
        serialized->phase > W30_PHASE_EFP_CONFIRM ||
        serialized->setup_partition >= serialized->config->partition_count ||
        !operation_valid(serialized, &serialized->active, 0) ||
        !operation_valid(serialized, &serialized->suspended_erase, 1) ||
        !operation_valid(serialized, &serialized->suspended_program, 1) ||
        (serialized->active.kind != W30_OP_NONE &&
         serialized->suspended_program.kind != W30_OP_NONE) ||
        (serialized->suspended_erase.kind != W30_OP_NONE &&
         serialized->suspended_program.kind != W30_OP_NONE) ||
        (serialized->suspended_erase.kind != W30_OP_NONE &&
         serialized->active.kind != W30_OP_NONE &&
         serialized->active.kind != W30_OP_PROGRAM) ||
        (serialized->phase != W30_PHASE_IDLE &&
         (serialized->active.kind != W30_OP_NONE ||
          serialized->suspended_erase.kind != W30_OP_NONE ||
          serialized->suspended_program.kind != W30_OP_NONE)) ||
        !cemu_nor_store_validate(patches, patch_count, serialized->config->size))
        return 0;
    for (unsigned i = 0; i < serialized->config->partition_count; i++)
        if (serialized->mode[i] < W30_MODE_ARRAY ||
            serialized->mode[i] > W30_MODE_STATUS ||
            (serialized->status[i] &
             ~(W30_SR_READY | W30_SR_CLEARABLE)))
            return 0;
    w30_state_t tmp = *serialized;
    memset(&tmp.store, 0, sizeof tmp.store);
    if (!cemu_nor_store_restore(&tmp.store, patches, patch_count,
                           serialized->config->size))
        return 0;
    cemu_w30_state_free(dst);
    *dst = tmp;
    return 1;
}

static void w30_periph_init(peripheral_t *p, w30_state_t *st,
                            const w30_config_t *config) {
    memset(p, 0, sizeof(*p));
    memset(st, 0, sizeof(*st));
    st->config = config;
    memset(st->customer, 0xFF, sizeof st->customer);
    cemu_w30_reset(st);
    p->id = "flash";
    p->model = config->model;
    p->state = st;
}

void cemu_w30_64mbit_top_periph_init(peripheral_t *p, w30_state_t *st) {
    w30_periph_init(p, st, &cemu_W30_64MBIT_TOP);
}

void cemu_w30_128mbit_top_periph_init(peripheral_t *p, w30_state_t *st) {
    w30_periph_init(p, st, &cemu_W30_128MBIT_TOP);
}
