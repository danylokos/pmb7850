/* Generic external NOR flash dispatch. The memory controller translates CPU
 * addresses; concrete flash models receive chip-relative offsets. */
#ifndef CEMU_PERIPH_FLASH_H
#define CEMU_PERIPH_FLASH_H

#include <stddef.h>
#include <stdint.h>

#include "flash_access.h"
#include "m58lw064d.h"
#include "am29lv.h"
#include "w30.h"

typedef enum {
    FLASH_MODEL_INVALID = 0,
    FLASH_MODEL_M58LW064D,
    FLASH_MODEL_AM29LV,
    FLASH_MODEL_W30,
} flash_model_kind_t;

typedef struct {
    flash_model_kind_t kind;
    union {
        m58lw064d_state_t m58;
        am29lv_state_t am29;
        w30_state_t w30;
    } u;
} flash_state_t;

typedef struct {
    const char *kind;
    uint32_t offset;
    uint32_t remaining_ticks;
    int partition;
    int suspended;
} flash_operation_info_t;

int cemu_flash_periph_init(peripheral_t *p, flash_state_t *st, const char *model);
void cemu_flash_configure_am29(flash_state_t *st, uint32_t protected_start,
                          uint32_t protected_size, int secsi_factory_locked);
void cemu_flash_reset(flash_state_t *st);
void cemu_flash_state_free(flash_state_t *st);
int cemu_flash_state_copy(flash_state_t *dst, const flash_state_t *src);
int cemu_flash_hex_parse(const char *text, uint8_t *bytes, size_t count);
int cemu_flash_factory_uid_set(flash_state_t *st, const uint8_t uid[8]);
int cemu_flash_has_factory_uid(const flash_state_t *st);
int cemu_flash_seed_bytes(const uint8_t *data, size_t len, flash_state_t *st,
                     uint32_t off, const uint8_t *bytes, size_t count);

uint8_t cemu_flash_read8(const uint8_t *data, size_t len, flash_state_t *st,
                    uint32_t off, int command_visible, flash_access_t *access);
uint8_t cemu_flash_peek8(const uint8_t *data, size_t len, const flash_state_t *st,
                    uint32_t off, int command_visible);
uint8_t cemu_flash_array_read8(const uint8_t *data, size_t len,
                          const flash_state_t *st, uint32_t off);
void cemu_flash_write8(const uint8_t *data, size_t len, flash_state_t *st,
                  uint32_t off, uint8_t value, int command_visible,
                  flash_access_t *access);
void cemu_flash_write16(const uint8_t *data, size_t len, flash_state_t *st,
                   uint32_t off, uint16_t value, int command_visible,
                   flash_access_t *access);

const char *cemu_flash_mode_str(const flash_state_t *st);
const char *cemu_flash_model_str(const flash_state_t *st);

/* Model-specific state access for serializers and identity configuration. */
m58lw064d_state_t *cemu_flash_m58_state(flash_state_t *st);
const m58lw064d_state_t *cemu_flash_m58_state_const(const flash_state_t *st);
am29lv_state_t *cemu_flash_am29_state(flash_state_t *st);
const am29lv_state_t *cemu_flash_am29_state_const(const flash_state_t *st);
w30_state_t *cemu_flash_w30_state(flash_state_t *st);
const w30_state_t *cemu_flash_w30_state_const(const flash_state_t *st);
int cemu_flash_operation_info(const flash_state_t *st, flash_operation_info_t *info);
void cemu_flash_emit_mutation(soc_t *s, peripheral_t *p,
                         flash_mutation_kind_t kind,
                         uint32_t offset, uint32_t size);

#endif
