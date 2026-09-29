#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m58lw064d.h"
#include "flash.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint16_t read_array16(const uint8_t *data, size_t len,
                             m58lw064d_state_t *st, uint32_t off) {
    uint16_t lo = cemu_m58lw064d_read8(data, len, st, off, 1, NULL);
    uint16_t hi = cemu_m58lw064d_read8(data, len, st, off + 1u, 1, NULL);
    return lo | (uint16_t)(hi << 8);
}

static void test_offsets_and_command_visibility(void) {
    uint8_t data[0x200];
    memset(data, 0xFF, sizeof(data));
    data[0x10] = 0xA5;
    peripheral_t p;
    m58lw064d_state_t st;
    m58lw064d_access_t access;
    cemu_m58lw064d_periph_init(&p, &st);

    CHECK(!strcmp(p.id, "flash"));
    CHECK(!strcmp(p.model, "m58lw064d"));
    CHECK(cemu_m58lw064d_read8(data, sizeof(data), &st, 0x10, 0, NULL) == 0xA5);

    cemu_m58lw064d_write8(data, sizeof(data), &st, 0, 0x90, 0, &access);
    CHECK(st.read_mode == FLASH_ARRAY);
    CHECK(!strcmp(access.subtype, "write-absorbed"));

    cemu_m58lw064d_write8(data, sizeof(data), &st, 0, 0x90, 1, &access);
    CHECK(st.read_mode == FLASH_ID);
    CHECK(cemu_m58lw064d_read8(data, sizeof(data), &st, 0, 1, NULL) == 0x20);
    CHECK(cemu_m58lw064d_read8(data, sizeof(data), &st, 0, 0, NULL) == 0xFF);
    cemu_m58lw064d_state_free(&st);
}

static void test_buffer_entries_are_offsets(void) {
    uint8_t data[0x40];
    memset(data, 0xFF, sizeof(data));
    peripheral_t p;
    m58lw064d_state_t st;
    cemu_m58lw064d_periph_init(&p, &st);
    cemu_m58lw064d_write8(data, sizeof(data), &st, 0x20, 0xE8, 1, NULL);
    cemu_m58lw064d_write16(data, sizeof(data), &st, 0x20, 0, 1, NULL);
    cemu_m58lw064d_write16(data, sizeof(data), &st, 0x20, 0x1234, 1, NULL);
    CHECK(st.write_buffer_len == 1);
    CHECK(st.write_buffer[0].off == 0x20);
    cemu_m58lw064d_write16(data, sizeof(data), &st, 0x20, 0xD0, 1, NULL);
    cemu_m58lw064d_write8(data, sizeof(data), &st, 0, 0xFF, 1, NULL);
    CHECK(cemu_m58lw064d_read8(data, sizeof(data), &st, 0x20, 1, NULL) == 0x34);
    CHECK(cemu_m58lw064d_read8(data, sizeof(data), &st, 0x21, 1, NULL) == 0x12);
    cemu_m58lw064d_state_free(&st);
}

static void test_single_program_and_status(void) {
    uint8_t data[0x200];
    memset(data, 0xFF, sizeof data);
    data[0x100] = 0xF0;
    peripheral_t p;
    m58lw064d_state_t st;
    cemu_m58lw064d_periph_init(&p, &st);

    cemu_m58lw064d_write8(data, sizeof data, &st, 0x100, 0x40, 1, NULL);
    CHECK(st.command_phase == FLASH_PHASE_PROGRAM_DATA);
    cemu_m58lw064d_write8(data, sizeof data, &st, 0x100, 0xC0, 1, NULL);
    CHECK(st.read_mode == FLASH_STATUS);
    CHECK(cemu_m58lw064d_read8(data, sizeof data, &st, 0, 1, NULL) == 0x80);
    cemu_m58lw064d_write8(data, sizeof data, &st, 0, 0xFF, 1, NULL);
    CHECK(cemu_m58lw064d_read8(data, sizeof data, &st, 0x100, 1, NULL) == 0xC0);

    /* NOR programming cannot turn a programmed zero back into one. */
    cemu_m58lw064d_write8(data, sizeof data, &st, 0x100, 0x10, 1, NULL);
    cemu_m58lw064d_write8(data, sizeof data, &st, 0x100, 0xFF, 1, NULL);
    CHECK(cemu_m58lw064d_read8(data, sizeof data, &st, 0, 1, NULL) == 0x90);
    cemu_m58lw064d_write8(data, sizeof data, &st, 0, 0x50, 1, NULL);
    CHECK(st.read_mode == FLASH_STATUS);
    CHECK(cemu_m58lw064d_read8(data, sizeof data, &st, 0, 1, NULL) == 0x80);
    cemu_m58lw064d_write8(data, sizeof data, &st, 0, 0xFF, 1, NULL);
    CHECK(cemu_m58lw064d_read8(data, sizeof data, &st, 0x100, 1, NULL) == 0xC0);
    cemu_m58lw064d_state_free(&st);
}

static void test_block_erase_and_reprogram(void) {
    size_t len = 3u * M58LW064D_BLOCK_SIZE;
    uint8_t *data = malloc(len);
    CHECK(data != NULL);
    if (!data) return;
    memset(data, 0xFF, len);
    uint32_t in_block = M58LW064D_BLOCK_SIZE + 0x24u;
    uint32_t adjacent = 2u * M58LW064D_BLOCK_SIZE + 0x24u;
    data[in_block] = 0x12;
    data[adjacent] = 0x34;
    peripheral_t p;
    m58lw064d_state_t st;
    cemu_m58lw064d_periph_init(&p, &st);

    cemu_m58lw064d_write8(data, len, &st, in_block + 1u, 0x40, 1, NULL);
    cemu_m58lw064d_write8(data, len, &st, in_block + 1u, 0x56, 1, NULL);
    cemu_m58lw064d_write8(data, len, &st, adjacent + 1u, 0x40, 1, NULL);
    cemu_m58lw064d_write8(data, len, &st, adjacent + 1u, 0x78, 1, NULL);
    CHECK(st.store.count == 2);

    cemu_m58lw064d_write8(data, len, &st, in_block, 0x20, 1, NULL);
    CHECK(st.command_phase == FLASH_PHASE_ERASE_CONFIRM);
    cemu_m58lw064d_write8(data, len, &st, in_block, 0xD0, 1, NULL);
    CHECK(st.erased_blocks == (UINT64_C(1) << 1));
    CHECK(st.store.count == 1);
    cemu_m58lw064d_write8(data, len, &st, 0, 0xFF, 1, NULL);
    CHECK(cemu_m58lw064d_read8(data, len, &st, in_block, 1, NULL) == 0xFF);
    CHECK(cemu_m58lw064d_read8(data, len, &st, in_block + 1u, 1, NULL) == 0xFF);
    CHECK(cemu_m58lw064d_read8(data, len, &st, adjacent, 1, NULL) == 0x34);
    CHECK(cemu_m58lw064d_read8(data, len, &st, adjacent + 1u, 1, NULL) == 0x78);

    cemu_m58lw064d_write8(data, len, &st, in_block, 0x40, 1, NULL);
    cemu_m58lw064d_write16(data, len, &st, in_block, 0x5AA5, 1, NULL);
    cemu_m58lw064d_write8(data, len, &st, 0, 0xFF, 1, NULL);
    CHECK(read_array16(data, len, &st, in_block) == 0x5AA5);

    cemu_m58lw064d_state_free(&st);
    free(data);
}

static void test_initial_seed_bypasses_nor_then_guest_rules_apply(void) {
    size_t len = M58LW064D_BLOCK_SIZE;
    uint8_t *data = malloc(len);
    CHECK(data != NULL);
    if (!data) return;
    memset(data, 0x00, len);
    peripheral_t p;
    m58lw064d_state_t st;
    cemu_m58lw064d_periph_init(&p, &st);

    const uint8_t seeded[] = {0xFF, 0x5A};
    CHECK(cemu_m58lw064d_seed_bytes(data, len, &st, 0x20, seeded, sizeof seeded));
    CHECK(cemu_m58lw064d_array_read8(data, len, &st, 0x20) == 0xFF);
    CHECK(cemu_m58lw064d_array_read8(data, len, &st, 0x21) == 0x5A);

    cemu_m58lw064d_write8(data, len, &st, 0x20, 0x40, 1, NULL);
    cemu_m58lw064d_write8(data, len, &st, 0x20, 0x0F, 1, NULL);
    cemu_m58lw064d_write8(data, len, &st, 0, 0xFF, 1, NULL);
    CHECK(cemu_m58lw064d_array_read8(data, len, &st, 0x20) == 0x0F);
    cemu_m58lw064d_write8(data, len, &st, 0x20, 0x40, 1, NULL);
    cemu_m58lw064d_write8(data, len, &st, 0x20, 0xFF, 1, NULL);
    CHECK(st.status == 0x90);
    cemu_m58lw064d_write8(data, len, &st, 0, 0xFF, 1, NULL);
    CHECK(cemu_m58lw064d_array_read8(data, len, &st, 0x20) == 0x0F);

    cemu_m58lw064d_write8(data, len, &st, 0x20, 0x20, 1, NULL);
    cemu_m58lw064d_write8(data, len, &st, 0x20, 0xD0, 1, NULL);
    cemu_m58lw064d_write8(data, len, &st, 0, 0xFF, 1, NULL);
    CHECK(cemu_m58lw064d_array_read8(data, len, &st, 0x20) == 0xFF);
    CHECK(cemu_m58lw064d_array_read8(data, len, &st, 0x21) == 0xFF);
    CHECK(st.store.count == 0);
    cemu_m58lw064d_state_free(&st);
    free(data);
}

static void test_invalid_sequences_do_not_mutate(void) {
    uint8_t data[0x100];
    memset(data, 0xFF, sizeof data);
    peripheral_t p;
    m58lw064d_state_t st;
    cemu_m58lw064d_periph_init(&p, &st);

    cemu_m58lw064d_write8(data, sizeof data, &st, 0x20, 0x20, 1, NULL);
    cemu_m58lw064d_write8(data, sizeof data, &st, 0x20, 0xD1, 1, NULL);
    CHECK(st.status == 0xB0);
    CHECK(st.erased_blocks == 0);
    cemu_m58lw064d_write8(data, sizeof data, &st, 0, 0x50, 1, NULL);
    CHECK(st.status == 0x80);

    cemu_m58lw064d_write8(data, sizeof data, &st, 0x20, 0xE8, 1, NULL);
    cemu_m58lw064d_write16(data, sizeof data, &st, 0x20, 16, 1, NULL);
    CHECK(st.status == 0xB0);
    CHECK(st.store.count == 0);
    cemu_m58lw064d_write8(data, sizeof data, &st, 0, 0x50, 1, NULL);

    cemu_m58lw064d_write8(data, sizeof data, &st, 0x1E, 0xE8, 1, NULL);
    cemu_m58lw064d_write16(data, sizeof data, &st, 0x1E, 1, 1, NULL);
    cemu_m58lw064d_write16(data, sizeof data, &st, 0x1E, 0x1234, 1, NULL);
    cemu_m58lw064d_write16(data, sizeof data, &st, 0x20, 0x5678, 1, NULL);
    CHECK(st.status == 0xB0);
    CHECK(st.store.count == 0);
    cemu_m58lw064d_write8(data, sizeof data, &st, 0, 0x50, 1, NULL);

    cemu_m58lw064d_write8(data, sizeof data, &st, 0x20, 0xE8, 1, NULL);
    cemu_m58lw064d_write16(data, sizeof data, &st, 0x20, 0, 1, NULL);
    cemu_m58lw064d_write16(data, sizeof data, &st, 0x20, 0x1234, 1, NULL);
    cemu_m58lw064d_write16(data, sizeof data, &st, 0x20, 0xD1, 1, NULL);
    CHECK(st.status == 0xB0);
    CHECK(st.store.count == 0);
    cemu_m58lw064d_write8(data, sizeof data, &st, 0, 0xFF, 1, NULL);
    CHECK(read_array16(data, sizeof data, &st, 0x20) == 0xFFFF);
    cemu_m58lw064d_state_free(&st);
}

static void test_state_copy_and_restore_validation(void) {
    uint8_t data[M58LW064D_BLOCK_SIZE + 1u];
    memset(data, 0, sizeof data);
    peripheral_t p;
    m58lw064d_state_t st;
    cemu_m58lw064d_periph_init(&p, &st);
    cemu_m58lw064d_write8(data, sizeof data, &st, 0, 0x20, 1, NULL);
    cemu_m58lw064d_write8(data, sizeof data, &st, 0, 0xD0, 1, NULL);
    cemu_m58lw064d_write8(data, sizeof data, &st, 0, 0x20, 1, NULL);
    st.status = 0xB0;

    m58lw064d_state_t copy;
    peripheral_t copy_p;
    cemu_m58lw064d_periph_init(&copy_p, &copy);
    CHECK(cemu_m58lw064d_state_copy(&copy, &st));
    CHECK(copy.command_phase == FLASH_PHASE_ERASE_CONFIRM);
    CHECK(copy.status == 0xB0);
    CHECK(copy.erased_blocks == 1);

    m58lw064d_patch_t unsorted[] = {{1, 0x12}, {0, 0x34}};
    CHECK(!cemu_m58lw064d_state_restore(
        &copy, FLASH_ARRAY, FLASH_PHASE_IDLE, 0x80, 0, 0,
        FLASH_WB_IDLE, 0, 0, UINT32_MAX, NULL, 0,
        unsorted, sizeof unsorted / sizeof unsorted[0]));
    CHECK(copy.command_phase == FLASH_PHASE_ERASE_CONFIRM);
    CHECK(copy.erased_blocks == 1);
    cemu_m58lw064d_state_free(&copy);
    cemu_m58lw064d_state_free(&st);
}

static void test_generic_dispatch(void) {
    uint8_t data[32];
    memset(data, 0xFF, sizeof data);
    data[0x10] = 0xA5;
    peripheral_t p;
    flash_state_t st;
    flash_access_t access;

    CHECK(cemu_flash_periph_init(&p, &st, "m58lw064d"));
    CHECK(st.kind == FLASH_MODEL_M58LW064D);
    CHECK(p.state == &st);
    CHECK(!strcmp(cemu_flash_model_str(&st), "m58lw064d"));
    CHECK(cemu_flash_read8(data, sizeof data, &st, 0x10, 1, NULL) == 0xA5);
    cemu_flash_write8(data, sizeof data, &st, 0, 0x90, 1, &access);
    CHECK(!strcmp(access.subtype, "cmd"));
    CHECK(!strcmp(cemu_flash_mode_str(&st), "id"));
    CHECK(cemu_flash_read8(data, sizeof data, &st, 0, 1, NULL) == 0x20);

    flash_state_t copy;
    memset(&copy, 0, sizeof copy);
    CHECK(cemu_flash_state_copy(&copy, &st));
    CHECK(copy.kind == FLASH_MODEL_M58LW064D);
    CHECK(cemu_flash_read8(data, sizeof data, &copy, 0, 1, NULL) == 0x20);
    cemu_flash_state_free(&copy);
    cemu_flash_state_free(&st);
}

int main(void) {
    test_offsets_and_command_visibility();
    test_buffer_entries_are_offsets();
    test_single_program_and_status();
    test_block_erase_and_reprogram();
    test_initial_seed_bypasses_nor_then_guest_rules_apply();
    test_invalid_sequences_do_not_mutate();
    test_state_copy_and_restore_validation();
    test_generic_dispatch();
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
