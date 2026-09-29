/* Shared AMD AM29LV x16 NOR flash command interface. */
#ifndef CEMU_PERIPH_AM29LV_H
#define CEMU_PERIPH_AM29LV_H

#include <stddef.h>
#include <stdint.h>

#include "flash_access.h"
#include "peripheral.h"
#include "nor_store.h"

#define AM29LV_SECSI_SIZE 256u
#define AM29LV_ESN_SIZE 16u
#define AM29LV_CUSTOMER_SIZE 8u
#define AM29LV_WRITE_BUFFER_WORDS 16u
#define AM29LV_SECTOR_SIZE 0x10000u
#define AM29LV_MAX_SECTOR_COUNT 256u
#define AM29LV_ERASE_TIMER_TICKS 8u
#define AM29LV_SECTOR_ERASE_TICKS 80000u

#define AM29LV_MANUFACTURER_ID 0x0001u
#define AM29LV_DEVICE_ID1      0x227Eu

#define AM29LV640MH_MODEL        "am29lv640mh"
#define AM29LV640MH_SECTOR_COUNT 128u
#define AM29LV640MH_CHIP_SIZE \
    (AM29LV640MH_SECTOR_COUNT * AM29LV_SECTOR_SIZE)
#define AM29LV640MH_DEVICE_ID2   0x220Cu
#define AM29LV640MH_DEVICE_ID3   0x2201u

#define AM29LV128MH_MODEL        "am29lv128mh"
#define AM29LV128MH_SECTOR_COUNT 256u
#define AM29LV128MH_CHIP_SIZE \
    (AM29LV128MH_SECTOR_COUNT * AM29LV_SECTOR_SIZE)
#define AM29LV128MH_DEVICE_ID2   0x2212u
#define AM29LV128MH_DEVICE_ID3   0x2200u

typedef struct {
    const char *model;
    uint32_t chip_size;
    uint16_t sector_count;
    uint16_t manufacturer_id;
    uint16_t device_id1;
    uint16_t device_id2;
    uint16_t device_id3;
} am29lv_config_t;

extern const am29lv_config_t cemu_AM29LV640MH_CONFIG;
extern const am29lv_config_t cemu_AM29LV128MH_CONFIG;

typedef enum {
    AM29_MODE_ARRAY,
    AM29_MODE_AUTOSELECT,
    AM29_MODE_SECSI,
} am29lv_mode_t;

typedef enum {
    AM29_PHASE_IDLE,
    AM29_PHASE_EXPECT_55,
    AM29_PHASE_EXPECT_COMMAND,
    AM29_PHASE_PROGRAM,
    AM29_PHASE_SECSI_EXIT_ZERO,
    AM29_PHASE_SECSI_PROTECT_40,
    AM29_PHASE_WRITE_BUFFER_COUNT,
    AM29_PHASE_WRITE_BUFFER_DATA,
    AM29_PHASE_WRITE_BUFFER_CONFIRM,
    AM29_PHASE_ERASE_UNLOCK_AA,
    AM29_PHASE_ERASE_UNLOCK_55,
    AM29_PHASE_ERASE_CONFIRM,
} am29lv_phase_t;

enum {
    AM29_TICK_NO_EVENT = 0,
    AM29_TICK_ERASE_TIMER = 1u << 0,
    AM29_TICK_ERASE_COMPLETE = 1u << 1,
};

typedef nor_patch_t am29lv_patch_t;

typedef struct {
    uint32_t off;
    uint16_t value;
    uint8_t size;
} am29lv_write_buffer_entry_t;

typedef flash_access_t am29lv_access_t;

typedef struct {
    const am29lv_config_t *config;
    am29lv_mode_t mode;
    am29lv_phase_t phase;
    uint32_t cmd_writes;
    uint32_t protected_start;
    uint32_t protected_end;
    uint8_t secsi[AM29LV_SECSI_SIZE];
    int secsi_esn_set;
    int secsi_locked;
    uint32_t write_buffer_sector;
    uint32_t write_buffer_page;
    uint8_t write_buffer_words_left;
    am29lv_write_buffer_entry_t
        write_buffer[AM29LV_WRITE_BUFFER_WORDS];
    size_t write_buffer_len;
    uint64_t erased_sectors[AM29LV_MAX_SECTOR_COUNT / 64u];
    int erase_active;
    int erase_suspended;
    uint32_t erase_sector;
    uint32_t erase_timer_ticks_remaining;
    uint32_t erase_ticks_remaining;
    int erase_toggle;
    nor_store_t store;
} am29lv_state_t;

void cemu_am29lv640mh_periph_init(peripheral_t *p, am29lv_state_t *st);
void cemu_am29lv128mh_periph_init(peripheral_t *p, am29lv_state_t *st);
void cemu_am29lv_periph_init(peripheral_t *p, am29lv_state_t *st,
                        const am29lv_config_t *config);
void cemu_am29lv_configure(am29lv_state_t *st,
                      uint32_t protected_start, uint32_t protected_size,
                      int secsi_factory_locked);
void cemu_am29lv_reset(am29lv_state_t *st);
void cemu_am29lv_state_free(am29lv_state_t *st);
int cemu_am29lv_state_copy(am29lv_state_t *dst,
                      const am29lv_state_t *src);
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
                         size_t patch_count);

uint32_t cemu_am29lv_tick(am29lv_state_t *st, uint32_t ticks);
uint64_t cemu_am29lv_next_event_ticks(const am29lv_state_t *st);
void cemu_am29lv_advance_quiet(am29lv_state_t *st, uint64_t ticks);

int cemu_am29lv_secsi_esn_parse(const char *text,
                           uint8_t esn[AM29LV_ESN_SIZE]);
int cemu_am29lv_secsi_customer_parse(
    const char *text, uint8_t customer[AM29LV_CUSTOMER_SIZE]);
void cemu_am29lv_secsi_esn_set(am29lv_state_t *st,
                          const uint8_t esn[AM29LV_ESN_SIZE]);
int cemu_am29lv_secsi_factory_set(am29lv_state_t *st, uint16_t offset,
                             const uint8_t *data, size_t len);
/* Seed exact initial main-array bytes before reset, bypassing guest NOR
 * programming rules.  Later guest writes still program only 1 -> 0. */
int cemu_am29lv_seed_bytes(const uint8_t *data, size_t len,
                      am29lv_state_t *st, uint32_t off,
                      const uint8_t *bytes, size_t count);

uint8_t cemu_am29lv_read8(const uint8_t *data, size_t len,
                     am29lv_state_t *st, uint32_t off,
                     int command_visible, am29lv_access_t *access);
uint8_t cemu_am29lv_peek8(const uint8_t *data, size_t len,
                     const am29lv_state_t *st, uint32_t off,
                     int command_visible);
uint8_t cemu_am29lv_array_read8(const uint8_t *data, size_t len,
                           const am29lv_state_t *st, uint32_t off);
void cemu_am29lv_write8(const uint8_t *data, size_t len,
                   am29lv_state_t *st, uint32_t off, uint8_t value,
                   int command_visible, am29lv_access_t *access);
void cemu_am29lv_write16(const uint8_t *data, size_t len,
                    am29lv_state_t *st, uint32_t off, uint16_t value,
                    int command_visible, am29lv_access_t *access);

const char *cemu_am29lv_mode_str(const am29lv_state_t *st);
const char *cemu_am29lv_phase_str(am29lv_phase_t phase);
const char *cemu_am29lv_model_str(const am29lv_state_t *st);
const am29lv_patch_t *
cemu_am29lv_state_patches(const am29lv_state_t *st, size_t *count_out);

#endif /* CEMU_PERIPH_AM29LV_H */
