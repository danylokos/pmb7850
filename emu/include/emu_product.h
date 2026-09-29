#ifndef EMU_PRODUCT_H
#define EMU_PRODUCT_H

#include "emu_engine.h"

#define EMU_MAX_LOGICAL_KEYS 24u
#define EMU_MAX_PRODUCT_CHIP_MODELS 2u

typedef enum {
    EMU_PRODUCT_KEYS    = UINT64_C(1) << 0,
    EMU_PRODUCT_DISPLAY = UINT64_C(1) << 1,
    EMU_PRODUCT_SERIAL  = UINT64_C(1) << 2,
    EMU_PRODUCT_SIM     = UINT64_C(1) << 3,
    EMU_PRODUCT_BATTERY = UINT64_C(1) << 4,
} emu_product_capability_t;

typedef struct {
    const char *name;
    uint16_t raw_code;
    uint8_t logical_code;
} emu_logical_key_t;

typedef struct {
    const char *role;
    const char *name;
    const char *default_model;
    const char *allowed_models[EMU_MAX_PRODUCT_CHIP_MODELS];
    size_t allowed_model_count;
    size_t size;
} emu_product_chip_t;

typedef struct {
    unsigned width;
    unsigned height;
    unsigned origin_x;
    unsigned origin_y;
    int mirror_x;
    int mirror_y;
} emu_display_geometry_t;

typedef struct {
    const char *name;
    const char *image_model;
    emu_product_chip_t chips[EMU_MAX_CHIPS];
    size_t chip_count;
    size_t primary_chip_index;
    size_t identity_chip_index;
    emu_display_geometry_t display;
    const emu_logical_key_t *keys;
    size_t key_count;
    uint64_t capabilities;
} emu_product_t;

size_t emu_product_count(void);
const emu_product_t *emu_product_at(size_t index);
const emu_product_t *emu_product_by_name(const char *name);
const emu_logical_key_t *emu_product_key(const emu_product_t *product,
                                         const char *name);
int emu_product_chip_model_allowed(const emu_product_chip_t *chip,
                                   const char *model);
emu_error_code_t emu_product_validate_prepared_storage(
    const emu_prepared_session_t *prepared, emu_error_t *error);
const char *emu_identity_kind_name(emu_identity_kind_t kind);

/* Populate product metadata and immutable physical chip views from a loaded
 * source. requested_device is NULL for metadata-driven selection. */
emu_error_code_t emu_product_prepare_image(emu_prepared_session_t *prepared,
                                            const char *requested_device,
                                            emu_error_t *error);

#endif
