/* M58LW064D NOR flash command interface — C port of
 * emu/src/soc/peripherals/flash.py.
 *
 * PARTIAL model: read-mode state machine plus the array-mutating program and
 * block-erase flows (datasheet Table 4 p.17). Program operations use NOR
 * semantics (1->0 only); erase restores one 128 KiB block to 0xFF. Operations
 * complete immediately. Timing, suspend/resume, protection, and CFI remain
 * unmodeled.
 *
 * The controller sees only chip-relative offsets. CPU address aliases and
 * command-cycle visibility are properties of the memory-controller map. */
#ifndef CEMU_PERIPH_M58LW064D_H
#define CEMU_PERIPH_M58LW064D_H

#include <stddef.h>
#include <stdint.h>
#include "flash_access.h"
#include "peripheral.h"
#include "nor_store.h"

/* ---- flash command interface (mirror flash.py FlashCommand) ----------- */
typedef enum { FLASH_ARRAY, FLASH_ID, FLASH_STATUS, FLASH_CFI } m58lw064d_mode_t;
typedef enum {
    FLASH_PHASE_IDLE,
    FLASH_PHASE_PROGRAM_DATA,
    FLASH_PHASE_ERASE_CONFIRM,
} m58lw064d_command_phase_t;
typedef enum {
    FLASH_WB_IDLE,
    FLASH_WB_EXPECT_COUNT,
    FLASH_WB_LOADING,
    FLASH_WB_EXPECT_CONFIRM,
} m58lw064d_write_buffer_state_t;

typedef nor_patch_t m58lw064d_patch_t;

typedef struct {
    uint32_t off;
    uint16_t value;
    uint8_t  size;
} m58lw064d_write_buffer_entry_t;

typedef flash_access_t m58lw064d_access_t;

#define M58LW064D_BLOCK_SIZE      (128u * 1024u)
#define M58LW064D_BLOCK_COUNT     64u
#define M58LW064D_WRITE_BUFFER_SIZE 32u
#define M58LW064D_WRITE_BUFFER_WORDS 16u

typedef struct {
    m58lw064d_mode_t read_mode;
    m58lw064d_command_phase_t command_phase;
    uint8_t      status;
    uint32_t     cmd_writes;
    uint64_t     erased_blocks;
    uint8_t      factory_uid[8];
    int          factory_uid_set;
    m58lw064d_write_buffer_state_t write_buffer_state;
    uint8_t      write_buffer_words_left;
    uint32_t     write_buffer_block;
    uint32_t     write_buffer_page;
    m58lw064d_write_buffer_entry_t *write_buffer;
    size_t                    write_buffer_len;
    size_t                    write_buffer_cap;
    nor_store_t                 store;
} m58lw064d_state_t;

/* Fill `p` (and its embedded `st`) as the flash peripheral. */
void cemu_m58lw064d_periph_init(peripheral_t *p, m58lw064d_state_t *st);
void cemu_m58lw064d_state_free(m58lw064d_state_t *st);
int cemu_m58lw064d_state_copy(m58lw064d_state_t *dst, const m58lw064d_state_t *src);
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
                        size_t patch_count);
/* Configure the M58LW064D factory-programmed 64-bit UID returned by Protection
 * Register words 0x81..0x84. Text form is exactly 16 hex digits in signature
 * address order (0x800102..0x800109), with an optional 0x prefix. */
int cemu_m58lw064d_factory_uid_parse(const char *text, uint8_t uid[8]);
void cemu_m58lw064d_factory_uid_set(m58lw064d_state_t *st, const uint8_t uid[8]);
/* Seed initial main-array bytes exactly, bypassing guest NOR 1->0 programming.
 * This is for immutable-image overlays applied before reset; subsequent guest
 * program/erase commands still operate on the resulting sparse patch state. */
int cemu_m58lw064d_seed_bytes(const uint8_t *data, size_t len,
                         m58lw064d_state_t *st, uint32_t off,
                         const uint8_t *bytes, size_t count);
const m58lw064d_patch_t *cemu_m58lw064d_state_patches(const m58lw064d_state_t *st, size_t *count_out);
const m58lw064d_write_buffer_entry_t *cemu_m58lw064d_state_write_buffer_entries(const m58lw064d_state_t *st,
                                                                   size_t *count_out);

/* One controller access at a chip-relative offset. `command_visible` is supplied
 * by the SoC mapping and is false for read-only aliases. */
uint8_t cemu_m58lw064d_read8(const uint8_t *data, size_t len,
                        m58lw064d_state_t *st, uint32_t off,
                        int command_visible, m58lw064d_access_t *access);
uint8_t cemu_m58lw064d_array_read8(const uint8_t *data, size_t len,
                              const m58lw064d_state_t *st, uint32_t off);
void cemu_m58lw064d_write8(const uint8_t *data, size_t len,
                      m58lw064d_state_t *st, uint32_t off, uint8_t value,
                      int command_visible, m58lw064d_access_t *access);
void cemu_m58lw064d_write16(const uint8_t *data, size_t len,
                       m58lw064d_state_t *st, uint32_t off, uint16_t value,
                       int command_visible, m58lw064d_access_t *access);

/* Read-mode string for flash-specific trace/debug metadata (array/id/status/cfi). */
const char *cemu_m58lw064d_mode_str(const m58lw064d_state_t *st);
const char *cemu_m58lw064d_command_phase_str(m58lw064d_command_phase_t phase);
const char *cemu_m58lw064d_write_buffer_state_str(m58lw064d_write_buffer_state_t state);

#endif /* CEMU_PERIPH_M58LW064D_H */
