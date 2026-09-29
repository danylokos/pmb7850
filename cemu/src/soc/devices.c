/* Private per-device board tables and prepared-description validation. */
#include "devices.h"

#include <stdio.h>
#include <string.h>

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#define MIB(n) ((uint32_t)(n) * 1024u * 1024u)

extern const device_config_t cemu_device_config_c55;
extern const device_config_t cemu_device_config_a52;
extern const device_config_t cemu_device_config_a55;
extern const device_config_t cemu_device_config_a60;
extern const device_config_t cemu_device_config_a62;
extern const device_config_t cemu_device_config_a65;
extern const device_config_t cemu_device_config_m55;
extern const device_config_t cemu_device_config_mc60;
extern const device_config_t cemu_device_config_c60;
extern const device_config_t cemu_device_config_cf62;
extern const device_config_t cemu_device_config_s55;
extern const device_config_t cemu_device_config_sl55;

static const device_config_t *const DEVICES[] = {
    &cemu_device_config_c55, &cemu_device_config_a52, &cemu_device_config_a55,
    &cemu_device_config_a60, &cemu_device_config_a62, &cemu_device_config_a65,
    &cemu_device_config_m55, &cemu_device_config_mc60, &cemu_device_config_c60,
    &cemu_device_config_cf62, &cemu_device_config_s55, &cemu_device_config_sl55,
};

static int validation_error(char *error, size_t error_size,
                            const char *message) {
    if (error && error_size) snprintf(error, error_size, "%s", message);
    return 0;
}

const device_config_t *cemu_device_by_name(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < COUNT_OF(DEVICES); i++) {
        const char *a = DEVICES[i]->name;
        const char *b = name;
        while (*a && *b) {
            char cb = *b;
            if ('A' <= cb && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (*a++ != cb) break;
            b++;
        }
        if (!*a && !*b) return DEVICES[i];
    }
    return NULL;
}

static int flash_logical_translate(const device_flash_config_t *flash,
                                   uint32_t address, int *chip_index) {
    for (int i = 0; i < flash->nwindows; i++) {
        const flash_window_config_t *window = &flash->windows[i];
        if (window->cpu_base <= address &&
            (uint64_t)address <
                (uint64_t)window->cpu_base + window->cpu_size) {
            if (!window->mirror_period) return 0;
            uint32_t offset = window->chip_base +
                ((address - window->cpu_base) % window->mirror_period);
            if (window->chip_index < 0 ||
                window->chip_index >= flash->nchips ||
                offset >= flash->chips[window->chip_index].chip_size)
                return 0;
            if (chip_index) *chip_index = window->chip_index;
            return 1;
        }
    }
    return 0;
}

static const char *canonical_flash_model(const char *model) {
    static const char *const models[] = {
        "m58lw064d", "am29lv640mh", "am29lv128mh",
        "w30-64mbit-top", "w30-128mbit-top",
    };
    for (size_t i = 0; model && i < COUNT_OF(models); i++)
        if (!strcmp(model, models[i])) return models[i];
    return NULL;
}

static void apply_primary_flash_engine(device_config_t *cfg, int chip_index,
                                       const char *engine) {
    flash_chip_config_t *chip = &cfg->flash.chips[chip_index];
    chip->model = engine;
    chip->protected_start = 0;
    chip->protected_size = 0;
    chip->secsi_factory_locked = 0;
    if (!strncmp(engine, "am29lv", 6)) {
        chip->protected_start =
            !strcmp(engine, "am29lv128mh") ? MIB(8) : 0;
        chip->protected_size = 0x040000;
        chip->secsi_factory_locked = 1;
        cfg->flash.eeprom_overlay_identity =
            (eeprom_overlay_identity_config_t){
                EEPROM_OVERLAY_IDENTITY_AM29_SECSI, chip_index
            };
    } else {
        cfg->flash.eeprom_overlay_identity =
            (eeprom_overlay_identity_config_t){
                EEPROM_OVERLAY_IDENTITY_FACTORY_UID, chip_index
            };
    }
}

int cemu_device_config_from_prepared(const char *name,
                                     const char *const chip_models[],
                                     const size_t chip_offsets[],
                                     const size_t chip_sizes[],
                                     size_t chip_count, size_t image_size,
                                     device_config_t *cfg,
                                     char *error, size_t error_size) {
    const device_config_t *base = cemu_device_by_name(name);
    if (!base || !chip_models || !chip_offsets || !chip_sizes || !cfg)
        return validation_error(error, error_size,
                                "invalid prepared device description");
    if (chip_count != (size_t)base->flash.nchips)
        return validation_error(error, error_size,
                                "prepared flash chip count disagrees with board");
    *cfg = *base;
    int primary = -1;
    if (!flash_logical_translate(&cfg->flash, 0, &primary) || primary < 0)
        return validation_error(error, error_size,
                                "logical reset address is not backed by flash");
    for (size_t i = 0; i < chip_count; i++) {
        const char *model = canonical_flash_model(chip_models[i]);
        if (!model)
            return validation_error(error, error_size,
                                    "prepared flash model is unsupported by CEMU");
        if (chip_sizes[i] != cfg->flash.chips[i].chip_size)
            return validation_error(error, error_size,
                                    "prepared flash chip size disagrees with board");
        if ((int)i != primary && strcmp(model, cfg->flash.chips[i].model))
            return validation_error(error, error_size,
                                    "prepared secondary flash model disagrees with board");
        cfg->flash_image.file_offsets[i] = chip_offsets[i];
        if ((int)i == primary) apply_primary_flash_engine(cfg, primary, model);
    }
    cfg->flash_image.count = (int)chip_count;
    if (!cemu_device_flash_config_validate(&cfg->flash, error, error_size) ||
        !cemu_device_flash_image_validate(cfg, image_size, error, error_size) ||
        !cemu_device_external_ram_config_validate(&cfg->external_ram,
                                             error, error_size))
        return 0;
    if (error && error_size) error[0] = 0;
    return 1;
}

int cemu_device_flash_config_validate(const device_flash_config_t *cfg,
                                 char *error, size_t error_size) {
    if (!cfg) return validation_error(error, error_size, "missing flash config");
    if (cfg->nchips <= 0 || cfg->nchips > MAX_FLASH_CHIPS)
        return validation_error(error, error_size, "invalid flash chip count");
    const eeprom_overlay_identity_config_t *identity =
        &cfg->eeprom_overlay_identity;
    if (identity->kind < EEPROM_OVERLAY_IDENTITY_NONE ||
        identity->kind > EEPROM_OVERLAY_IDENTITY_AM29_SECSI)
        return validation_error(error, error_size,
                                "invalid EEPROM overlay identity kind");
    if (identity->kind != EEPROM_OVERLAY_IDENTITY_NONE &&
        (identity->chip_index < 0 || identity->chip_index >= cfg->nchips))
        return validation_error(error, error_size,
                                "invalid EEPROM overlay identity chip");
    for (int i = 0; i < cfg->nchips; i++) {
        const flash_chip_config_t *chip = &cfg->chips[i];
        if (!chip->name || !chip->name[0])
            return validation_error(error, error_size, "missing flash chip name");
        if (!chip->model || !chip->model[0])
            return validation_error(error, error_size,
                                    "missing flash controller model");
        if (!chip->chip_size)
            return validation_error(error, error_size, "zero flash chip size");
        for (int j = 0; j < i; j++)
            if (!strcmp(chip->name, cfg->chips[j].name))
                return validation_error(error, error_size,
                                        "duplicate flash chip name");
    }
    if (identity->kind == EEPROM_OVERLAY_IDENTITY_FACTORY_UID &&
        strcmp(cfg->chips[identity->chip_index].model, "m58lw064d") &&
        strcmp(cfg->chips[identity->chip_index].model, "w30-64mbit-top") &&
        strcmp(cfg->chips[identity->chip_index].model, "w30-128mbit-top"))
        return validation_error(error, error_size,
                                "EEPROM overlay identity requires UID-capable flash");
    if (identity->kind == EEPROM_OVERLAY_IDENTITY_AM29_SECSI) {
        const char *model = cfg->chips[identity->chip_index].model;
        if (strcmp(model, "am29lv128mh") && strcmp(model, "am29lv640mh"))
            return validation_error(error, error_size,
                                    "EEPROM overlay identity requires AM29 flash");
    }
    if (cfg->nwindows <= 0 || cfg->nwindows > MAX_FLASH_WINDOWS)
        return validation_error(error, error_size, "invalid flash window count");
    for (int i = 0; i < cfg->nwindows; i++) {
        const flash_window_config_t *window = &cfg->windows[i];
        if (window->chip_index < 0 || window->chip_index >= cfg->nchips)
            return validation_error(error, error_size,
                                    "invalid flash window chip index");
        const flash_chip_config_t *chip = &cfg->chips[window->chip_index];
        uint64_t cpu_end = (uint64_t)window->cpu_base + window->cpu_size;
        if (!window->cpu_size)
            return validation_error(error, error_size,
                                    "zero flash CPU window size");
        if (!window->mirror_period)
            return validation_error(error, error_size,
                                    "zero flash mirror period");
        if (cpu_end > 0x1000000ull)
            return validation_error(error, error_size,
                                    "flash CPU window overflows address space");
        uint32_t span = window->cpu_size < window->mirror_period
                      ? window->cpu_size : window->mirror_period;
        if ((uint64_t)window->chip_base + span > chip->chip_size)
            return validation_error(error, error_size,
                                    "flash window exceeds chip bounds");
        for (int j = 0; j < i; j++) {
            const flash_window_config_t *prior = &cfg->windows[j];
            uint64_t prior_end = (uint64_t)prior->cpu_base + prior->cpu_size;
            if ((uint64_t)window->cpu_base < prior_end &&
                (uint64_t)prior->cpu_base < cpu_end)
                return validation_error(error, error_size,
                                        "overlapping flash CPU windows");
        }
    }
    if (error && error_size) error[0] = 0;
    return 1;
}

int cemu_device_flash_image_validate(const device_config_t *cfg,
                                size_t image_size,
                                char *error, size_t error_size) {
    if (!cfg)
        return validation_error(error, error_size, "missing device config");
    if (!cemu_device_flash_config_validate(&cfg->flash, error, error_size))
        return 0;
    if (cfg->flash_image.count != cfg->flash.nchips)
        return validation_error(error, error_size,
                                "flash image mapping is unresolved");
    uint64_t expected_size = 0;
    uint64_t physical_size = 0;
    for (int i = 0; i < cfg->flash.nchips; i++) {
        const flash_chip_config_t *chip = &cfg->flash.chips[i];
        uint64_t start = cfg->flash_image.file_offsets[i];
        uint64_t end = start + chip->chip_size;
        physical_size += chip->chip_size;
        if (end > expected_size) expected_size = end;
        for (int j = 0; j < i; j++) {
            uint64_t prior_start = cfg->flash_image.file_offsets[j];
            uint64_t prior_end = prior_start + cfg->flash.chips[j].chip_size;
            if (start < prior_end && prior_start < end)
                return validation_error(error, error_size,
                                        "overlapping flash chip image ranges");
        }
    }
    if (physical_size != image_size || expected_size != image_size)
        return validation_error(
            error, error_size,
            image_size > physical_size || image_size > expected_size
                ? "flash image exceeds configured physical flash size"
                : "flash image is smaller than configured physical flash size");
    if (error && error_size) error[0] = 0;
    return 1;
}

int cemu_device_external_ram_config_validate(const device_external_ram_config_t *cfg,
                                        char *error, size_t error_size) {
    if (!cfg)
        return validation_error(error, error_size, "missing external RAM config");
    if (cfg->count < 0 || cfg->count > MAX_EXTERNAL_RAM_DEVICES)
        return validation_error(error, error_size,
                                "invalid external RAM device count");
    for (int i = 0; i < cfg->count; i++) {
        const external_ram_config_t *ram = &cfg->devices[i];
        if (!ram->model || !ram->model[0])
            return validation_error(error, error_size,
                                    "missing external RAM model");
        if (!ram->chip_size)
            return validation_error(error, error_size,
                                    "zero external RAM chip size");
        if (ram->addrsel_index < 1 || ram->addrsel_index > 4)
            return validation_error(error, error_size,
                                    "invalid external RAM ADDRSEL index");
        for (int j = 0; j < i; j++)
            if (cfg->devices[j].addrsel_index == ram->addrsel_index)
                return validation_error(error, error_size,
                                        "duplicate external RAM ADDRSEL index");
    }
    if (error && error_size) error[0] = 0;
    return 1;
}
