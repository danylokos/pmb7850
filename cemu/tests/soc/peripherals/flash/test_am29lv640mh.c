#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "am29lv.h"
#include "flash.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void unlock_command(const uint8_t *data, size_t len,
                           am29lv_state_t *st, uint8_t command) {
    cemu_am29lv_write8(data, len, st, 0xAAA, 0xAA, 1, NULL);
    cemu_am29lv_write8(data, len, st, 0x555, 0x55, 1, NULL);
    cemu_am29lv_write8(data, len, st, 0xAAA, command, 1, NULL);
}

static void sector_erase(const uint8_t *data, size_t len,
                         am29lv_state_t *st, uint32_t sector) {
    unlock_command(data, len, st, 0x80);
    cemu_am29lv_write8(data, len, st, 0xAAA, 0xAA, 1, NULL);
    cemu_am29lv_write8(data, len, st, 0x555, 0x55, 1, NULL);
    cemu_am29lv_write8(data, len, st, sector, 0x30, 1, NULL);
}

static void test_geometry_ids_and_copy(void) {
    const size_t len = AM29LV640MH_CHIP_SIZE + AM29LV_SECTOR_SIZE;
    uint8_t *data = malloc(len);
    CHECK(data != NULL);
    if (!data) return;
    memset(data, 0x00, len);

    peripheral_t p;
    flash_state_t flash;
    CHECK(cemu_flash_periph_init(&p, &flash, AM29LV640MH_MODEL));
    am29lv_state_t *st = &flash.u.am29;
    CHECK(st->config != NULL);
    CHECK(st->config == &cemu_AM29LV640MH_CONFIG);
    CHECK(st->config->chip_size == AM29LV640MH_CHIP_SIZE);
    CHECK(st->config->sector_count == AM29LV640MH_SECTOR_COUNT);
    CHECK(!strcmp(p.model, AM29LV640MH_MODEL));
    CHECK(!strcmp(cemu_flash_model_str(&flash), AM29LV640MH_MODEL));

    unlock_command(data, len, st, 0x90);
    CHECK(cemu_am29lv_read8(data, len, st, 0x00, 1, NULL) == 0x01);
    CHECK(cemu_am29lv_read8(data, len, st, 0x02, 1, NULL) == 0x7E);
    CHECK(cemu_am29lv_read8(data, len, st, 0x03, 1, NULL) == 0x22);
    CHECK(cemu_am29lv_read8(data, len, st, 0x1C, 1, NULL) == 0x0C);
    CHECK(cemu_am29lv_read8(data, len, st, 0x1D, 1, NULL) == 0x22);
    CHECK(cemu_am29lv_read8(data, len, st, 0x1E, 1, NULL) == 0x01);
    CHECK(cemu_am29lv_read8(data, len, st, 0x1F, 1, NULL) == 0x22);
    cemu_am29lv_reset(st);

    const uint32_t final_sector =
        (AM29LV640MH_SECTOR_COUNT - 1u) * AM29LV_SECTOR_SIZE;
    sector_erase(data, len, st, final_sector);
    CHECK(st->erase_active);
    CHECK(st->erase_sector == final_sector);
    cemu_am29lv_reset(st);

    sector_erase(data, len, st, AM29LV640MH_CHIP_SIZE);
    CHECK(!st->erase_active);
    CHECK(st->phase == AM29_PHASE_IDLE);

    flash_state_t copy;
    memset(&copy, 0, sizeof copy);
    CHECK(cemu_flash_state_copy(&copy, &flash));
    CHECK(copy.u.am29.config == st->config);
    CHECK(copy.u.am29.config == &cemu_AM29LV640MH_CONFIG);
    CHECK(copy.u.am29.config->chip_size == AM29LV640MH_CHIP_SIZE);
    CHECK(copy.u.am29.config->sector_count == AM29LV640MH_SECTOR_COUNT);
    CHECK(copy.u.am29.config->device_id2 == 0x220C);
    CHECK(copy.u.am29.config->device_id3 == 0x2201);
    CHECK(!strcmp(cemu_flash_model_str(&copy), AM29LV640MH_MODEL));

    cemu_flash_state_free(&copy);
    cemu_flash_state_free(&flash);
    free(data);
}

int main(void) {
    test_geometry_ids_and_copy();
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
