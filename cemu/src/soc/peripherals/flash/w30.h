/* Intel W30-family top-parameter NOR command engine. */
#ifndef CEMU_PERIPH_W30_H
#define CEMU_PERIPH_W30_H

#include <stddef.h>
#include <stdint.h>

#include "flash_access.h"
#include "nor_store.h"
#include "peripheral.h"

#define W30_MANUFACTURER_ID                 0x0089u
#define W30_PARTITION_SIZE                  0x080000u
#define W30_MAIN_BLOCK_SIZE                 0x010000u
#define W30_PARAMETER_BLOCK_SIZE            0x002000u
#define W30_PARAMETER_BLOCK_COUNT           8u

#define W30_64MBIT_TOP_MODEL                "w30-64mbit-top"
#define W30_64MBIT_TOP_SIZE                 0x800000u
#define W30_64MBIT_TOP_DEVICE_ID            0x8854u
#define W30_64MBIT_TOP_CFI_DEVICE_SIZE      0x17u
#define W30_64MBIT_TOP_CFI_ERASE_REGIONS    0x11u
#define W30_64MBIT_TOP_PARTITION_COUNT      16u
#define W30_64MBIT_TOP_MAIN_BLOCK_COUNT     127u
#define W30_64MBIT_TOP_PARAMETER_BASE       0x7F0000u
#define W30_64MBIT_TOP_BLOCK_COUNT          135u

#define W30_128MBIT_TOP_MODEL               "w30-128mbit-top"
#define W30_128MBIT_TOP_SIZE                0x1000000u
#define W30_128MBIT_TOP_DEVICE_ID           0x8856u
#define W30_128MBIT_TOP_CFI_DEVICE_SIZE     0x18u
#define W30_128MBIT_TOP_CFI_ERASE_REGIONS   0x21u
#define W30_128MBIT_TOP_PARTITION_COUNT     32u
#define W30_128MBIT_TOP_MAIN_BLOCK_COUNT    255u
#define W30_128MBIT_TOP_PARAMETER_BASE      0xFF0000u
#define W30_128MBIT_TOP_BLOCK_COUNT         263u

#define W30_MAX_PARTITION_COUNT W30_128MBIT_TOP_PARTITION_COUNT
#define W30_MAX_BLOCK_COUNT     W30_128MBIT_TOP_BLOCK_COUNT
#define W30_MAX_BLOCK_BITMAP_WORDS \
    ((W30_MAX_BLOCK_COUNT + 63u) / 64u)
#define W30_PROGRAM_TICKS 8u
#define W30_ERASE_TICKS   80000u

typedef struct {
    const char *model;
    uint32_t size;
    uint16_t manufacturer_id;
    uint16_t device_id;
    uint8_t cfi_device_size;
    uint8_t cfi_erase_region_count;
    uint8_t partition_count;
    uint16_t main_block_count;
    uint32_t parameter_base;
    uint16_t block_count;
} w30_config_t;

extern const w30_config_t cemu_W30_64MBIT_TOP;
extern const w30_config_t cemu_W30_128MBIT_TOP;

typedef enum {
    W30_MODE_ARRAY,
    W30_MODE_ID,
    W30_MODE_CFI,
    W30_MODE_STATUS,
} w30_mode_t;

typedef enum {
    W30_PHASE_IDLE,
    W30_PHASE_PROGRAM_DATA,
    W30_PHASE_ERASE_CONFIRM,
    W30_PHASE_LOCK_CONFIRM,
    W30_PHASE_PROTECTION_DATA,
    W30_PHASE_EFP_CONFIRM,
} w30_phase_t;

typedef enum {
    W30_OP_NONE,
    W30_OP_PROGRAM,
    W30_OP_ERASE,
    W30_OP_PROTECTION_PROGRAM,
} w30_operation_kind_t;

typedef struct {
    w30_operation_kind_t kind;
    uint8_t partition;
    uint16_t block;
    uint32_t off;
    uint16_t value;
    uint32_t ticks_remaining;
    uint8_t program_error;
} w30_operation_t;

typedef flash_access_t w30_access_t;

enum {
    W30_TICK_NO_EVENT = 0,
    W30_TICK_PROGRAM_COMPLETE = 1u << 0,
    W30_TICK_ERASE_COMPLETE = 1u << 1,
    W30_TICK_PROTECTION_COMPLETE = 1u << 2,
};

typedef struct {
    const w30_config_t *config;
    w30_mode_t mode[W30_MAX_PARTITION_COUNT];
    uint8_t status[W30_MAX_PARTITION_COUNT];
    uint16_t configuration[W30_MAX_PARTITION_COUNT];
    w30_phase_t phase;
    uint8_t setup_partition;
    uint32_t setup_off;
    uint32_t cmd_writes;
    uint64_t erased_blocks[W30_MAX_BLOCK_BITMAP_WORDS];
    uint64_t locked_blocks[W30_MAX_BLOCK_BITMAP_WORDS];
    uint64_t lockdown_blocks[W30_MAX_BLOCK_BITMAP_WORDS];
    uint8_t factory_uid[8];
    int factory_uid_set;
    uint8_t customer[8];
    int customer_locked;
    w30_operation_t active;
    w30_operation_t suspended_erase;
    w30_operation_t suspended_program;
    nor_store_t store;
} w30_state_t;

void cemu_w30_64mbit_top_periph_init(peripheral_t *p, w30_state_t *st);
void cemu_w30_128mbit_top_periph_init(peripheral_t *p, w30_state_t *st);
void cemu_w30_reset(w30_state_t *st);
void cemu_w30_state_free(w30_state_t *st);
int cemu_w30_state_copy(w30_state_t *dst, const w30_state_t *src);
int cemu_w30_state_restore(w30_state_t *dst,
                      const w30_state_t *serialized,
                      const nor_patch_t *patches, size_t patch_count);

int cemu_w30_seed_bytes(const uint8_t *data, size_t len,
                   w30_state_t *st, uint32_t off,
                   const uint8_t *bytes, size_t count);
uint8_t cemu_w30_array_read8(const uint8_t *data, size_t len,
                        const w30_state_t *st, uint32_t off);
uint8_t cemu_w30_read8(const uint8_t *data, size_t len,
                  w30_state_t *st, uint32_t off,
                  int command_visible, w30_access_t *access);
uint8_t cemu_w30_peek8(const uint8_t *data, size_t len,
                  const w30_state_t *st, uint32_t off,
                  int command_visible);
void cemu_w30_write8(const uint8_t *data, size_t len,
                w30_state_t *st, uint32_t off,
                uint8_t value, int command_visible,
                w30_access_t *access);
void cemu_w30_write16(const uint8_t *data, size_t len,
                 w30_state_t *st, uint32_t off,
                 uint16_t value, int command_visible,
                 w30_access_t *access);

uint32_t cemu_w30_tick(w30_state_t *st, uint32_t ticks);
uint64_t cemu_w30_next_event_ticks(const w30_state_t *st);
void cemu_w30_advance_quiet(w30_state_t *st, uint64_t ticks);

const char *cemu_w30_mode_str(const w30_state_t *st);
const char *cemu_w30_phase_str(w30_phase_t phase);
const char *cemu_w30_operation_str(w30_operation_kind_t kind);
int cemu_w30_block_for_offset(const w30_state_t *st, uint32_t off,
                         uint32_t *start, uint32_t *size);
unsigned cemu_w30_block_bitmap_words(const w30_state_t *st);
const char *cemu_w30_model_str(const w30_state_t *st);
void cemu_w30_factory_uid_set(w30_state_t *st, const uint8_t uid[8]);

#endif
