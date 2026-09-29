#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash.h"
#include "w30.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint16_t read16(uint8_t *data, w30_state_t *st,
                       uint32_t off, int visible) {
    return cemu_w30_read8(
               data, st->config->size, st, off, visible, NULL) |
        ((uint16_t)cemu_w30_read8(
            data, st->config->size, st, off + 1u, visible, NULL) << 8);
}

static void command(uint8_t *data, w30_state_t *st,
                    uint32_t off, uint8_t cmd) {
    cemu_w30_write8(
        data, st->config->size, st, off, cmd, 1, NULL);
}

static void unlock(uint8_t *data, w30_state_t *st,
                   uint32_t off) {
    command(data, st, off, 0x60);
    command(data, st, off, 0xD0);
}

static void test_model_names(void) {
    peripheral_t p;
    flash_state_t flash;

    CHECK(cemu_flash_periph_init(&p, &flash, W30_64MBIT_TOP_MODEL));
    CHECK(flash.kind == FLASH_MODEL_W30);
    CHECK(cemu_flash_w30_state(&flash)->config == &cemu_W30_64MBIT_TOP);
    CHECK(!strcmp(cemu_flash_model_str(&flash), W30_64MBIT_TOP_MODEL));
    cemu_flash_state_free(&flash);

    CHECK(cemu_flash_periph_init(&p, &flash, W30_128MBIT_TOP_MODEL));
    CHECK(flash.kind == FLASH_MODEL_W30);
    CHECK(cemu_flash_w30_state(&flash)->config == &cemu_W30_128MBIT_TOP);
    CHECK(!strcmp(cemu_flash_model_str(&flash), W30_128MBIT_TOP_MODEL));
    cemu_flash_state_free(&flash);

    CHECK(!cemu_flash_periph_init(&p, &flash, "rd28f6408w30t"));
}

static void test_geometry(void) {
    peripheral_t p;
    w30_state_t st;
    cemu_w30_64mbit_top_periph_init(&p, &st);
    uint32_t start, size;
    CHECK(cemu_w30_block_for_offset(&st, 0, &start, &size) == 0);
    CHECK(start == 0 && size == 0x10000);
    CHECK(cemu_w30_block_for_offset(&st, 0x38FFFF, &start, &size) == 0x38);
    CHECK(cemu_w30_block_for_offset(&st, 0x390000, &start, &size) == 0x39);
    CHECK(cemu_w30_block_for_offset(&st, 0x7EFFFF, &start, &size) == 126);
    CHECK(cemu_w30_block_for_offset(&st, 0x7F0000, &start, &size) == 127);
    CHECK(size == 0x2000);
    for (unsigned i = 0; i < W30_64MBIT_TOP_PARTITION_COUNT; i++) {
        uint32_t boundary = i * W30_PARTITION_SIZE;
        CHECK(boundary == 0 ||
              cemu_w30_block_for_offset(
                  &st, boundary - 1u, NULL, NULL) !=
              cemu_w30_block_for_offset(&st, boundary, NULL, NULL));
    }
    for (unsigned i = 0; i < W30_PARAMETER_BLOCK_COUNT; i++) {
        uint32_t off = W30_64MBIT_TOP_PARAMETER_BASE +
                       i * W30_PARAMETER_BLOCK_SIZE;
        CHECK(cemu_w30_block_for_offset(&st, off, &start, &size) ==
              (int)(W30_64MBIT_TOP_MAIN_BLOCK_COUNT + i));
        CHECK(start == off && size == W30_PARAMETER_BLOCK_SIZE);
    }
    CHECK(cemu_w30_block_for_offset(
              &st, W30_64MBIT_TOP_SIZE, NULL, NULL) < 0);
    cemu_w30_state_free(&st);
}

static void test_ids_cfi_locks_and_alias(void) {
    uint8_t *data = malloc(W30_64MBIT_TOP_SIZE);
    CHECK(data != NULL);
    if (!data) return;
    memset(data, 0xA5, W30_64MBIT_TOP_SIZE);
    peripheral_t p;
    w30_state_t st;
    cemu_w30_64mbit_top_periph_init(&p, &st);

    command(data, &st, 0x390000, 0x90);
    CHECK(read16(data, &st, 0x390000, 1) == W30_MANUFACTURER_ID);
    CHECK(read16(data, &st, 0x390002, 1) ==
          W30_64MBIT_TOP_DEVICE_ID);
    CHECK(read16(data, &st, 0x390004, 1) == 1);
    CHECK(cemu_w30_read8(data, W30_64MBIT_TOP_SIZE, &st, 0x390000, 0, NULL) ==
          0xA5);
    uint8_t uid[8] = {0,1,2,3,4,5,6,7};
    cemu_w30_factory_uid_set(&st, uid);
    command(data, &st, 0x380000, 0x90);
    for (unsigned i = 0; i < 8; i++)
        CHECK(cemu_w30_read8(
                  data, W30_64MBIT_TOP_SIZE, &st,
                  0x380102 + i, 1, NULL) == i);

    unlock(data, &st, 0x390000);
    command(data, &st, 0x390000, 0x90);
    CHECK(read16(data, &st, 0x390004, 1) == 0);
    command(data, &st, 0x390000, 0x60);
    command(data, &st, 0x390000, 0x2F);
    command(data, &st, 0x390000, 0x90);
    CHECK(read16(data, &st, 0x390004, 1) == 3);
    unlock(data, &st, 0x390000);
    command(data, &st, 0x390000, 0x90);
    CHECK(read16(data, &st, 0x390004, 1) == 2);

    static const uint8_t expected[0x78] = {
        [0x10]='Q',[0x11]='R',[0x12]='Y',[0x13]=3,[0x15]=0x39,
        [0x1B]=0x17,[0x1C]=0x19,[0x1D]=0xB4,[0x1E]=0xC6,
        [0x1F]=4,[0x21]=0x0A,[0x23]=4,[0x25]=3,[0x27]=0x17,
        [0x28]=1,[0x2C]=0x11,[0x2D]=7,[0x30]=1,[0x31]=6,
        [0x34]=1,[0x35]=7,[0x37]=0x20,[0x39]='P',[0x3A]='R',
        [0x3B]='I',[0x3C]='1',[0x3D]='3',[0x3E]=0xE6,[0x3F]=3,
        [0x42]=1,[0x43]=3,[0x45]=0x18,[0x46]=0xC0,[0x47]=1,
        [0x48]=0x80,[0x4A]=3,[0x4B]=3,[0x4C]=3,[0x4D]=3,
        [0x4E]=1,[0x4F]=2,[0x50]=7,[0x51]=2,[0x52]=0x0F,
        [0x54]=1,[0x57]=0x0F,[0x58]=7,[0x5B]=1,[0x5C]=0x64,
        [0x5E]=1,[0x5F]=3,[0x60]=1,[0x62]=1,[0x65]=2,
        [0x66]=6,[0x69]=1,[0x6A]=0x64,[0x6C]=1,[0x6D]=3,
        [0x6E]=7,[0x70]=0x20,[0x72]=0x64,[0x74]=1,[0x75]=2,
    };
    command(data, &st, 0x400000, 0x98);
    for (unsigned i = 0; i < sizeof expected; i++)
        CHECK(cemu_w30_read8(
                  data, W30_64MBIT_TOP_SIZE, &st,
                  0x400000 + i * 2u, 1, NULL) ==
              expected[i]);

    command(data, &st, 0x2468, 0x60);
    command(data, &st, 0x2468, 0x03);
    CHECK(st.configuration[0] == 0x1234);
    command(data, &st, 0, 0x90);
    CHECK(read16(data, &st, 0x0A, 1) == 0x1234);
    cemu_w30_reset(&st);
    CHECK(st.configuration[0] == 0x1234);
    cemu_w30_state_free(&st);
    free(data);
}

static void test_program_erase_suspend_nested_and_reset(void) {
    uint8_t *data = calloc(1, W30_64MBIT_TOP_SIZE);
    CHECK(data != NULL);
    if (!data) return;
    data[0x38FAF2] = 0x10;
    data[0x38FAF3] = 0x00;
    data[0x390020] = 0x12;
    data[0x7F1BA4] = 0x34;
    data[0x7F4000] = 0x56;
    peripheral_t p;
    flash_state_t flash;
    CHECK(cemu_flash_periph_init(&p, &flash, W30_64MBIT_TOP_MODEL));
    w30_state_t *st = cemu_flash_w30_state(&flash);

    unlock(data, st, 0x390000);
    command(data, st, 0x390000, 0x20);
    command(data, st, 0x390000, 0xD0);
    CHECK(st->active.kind == W30_OP_ERASE);
    command(data, st, 0x390000, 0x98);
    CHECK(read16(data, st, 0x390020, 1) == 0);
    command(data, st, 0x000000, 0x70);
    CHECK(read16(data, st, 0, 1) == 0x0001);
    command(data, st, 0x390000, 0x70);
    CHECK(read16(data, st, 0x390000, 1) == 0);
    CHECK(cemu_w30_array_read8(
              data, W30_64MBIT_TOP_SIZE, st, 0x390020) == 0x12);
    CHECK(cemu_w30_tick(st, W30_ERASE_TICKS - 1) == 0);
    CHECK(cemu_w30_tick(st, 1) == W30_TICK_ERASE_COMPLETE);
    CHECK(cemu_w30_array_read8(
              data, W30_64MBIT_TOP_SIZE, st, 0x390020) == 0xFF);
    CHECK(read16(data, st, 0x38FAF2, 0) == 0x0010);

    unlock(data, st, 0x7F4000);
    command(data, st, 0x7F4000, 0x20);
    command(data, st, 0x7F4000, 0xD0);
    cemu_w30_tick(st, 123);
    uint32_t remaining = st->active.ticks_remaining;
    command(data, st, 0x7F4000, 0xB0);
    CHECK(st->active.kind == W30_OP_NONE);
    CHECK(st->suspended_erase.kind == W30_OP_ERASE);
    CHECK((read16(data, st, 0x780000, 1) & 0xC1) == 0xC0);

    unlock(data, st, 0x780000);
    command(data, st, 0x780100, 0x40);
    cemu_w30_write16(data, W30_64MBIT_TOP_SIZE, st, 0x780100,
                0x1234, 1, NULL);
    CHECK(st->active.kind == W30_OP_PROGRAM);
    CHECK(read16(data, st, 0x780100, 0) == 0);
    CHECK(cemu_w30_tick(st, W30_PROGRAM_TICKS) ==
          W30_TICK_PROGRAM_COMPLETE);
    CHECK(read16(data, st, 0x780100, 0) == 0);
    CHECK(st->suspended_erase.kind == W30_OP_ERASE);
    command(data, st, 0x10000, 0xD0);
    CHECK(st->active.kind == W30_OP_ERASE);
    CHECK(st->active.ticks_remaining == remaining);
    CHECK(cemu_w30_tick(st, remaining) == W30_TICK_ERASE_COMPLETE);
    CHECK(cemu_w30_array_read8(
              data, W30_64MBIT_TOP_SIZE, st, 0x7F4000) == 0xFF);
    CHECK(cemu_w30_array_read8(
              data, W30_64MBIT_TOP_SIZE, st, 0x7F1BA4) == 0x34);

    unlock(data, st, 0x10000);
    command(data, st, 0x10000, 0x40);
    cemu_w30_write16(data, W30_64MBIT_TOP_SIZE, st, 0x10000,
                0xFFFF, 1, NULL);
    cemu_w30_tick(st, W30_PROGRAM_TICKS);
    CHECK(st->status[0] & 0x10);
    command(data, st, 0, 0x50);
    CHECK((st->status[0] & 0x3A) == 0);

    command(data, st, 0, 0x30);
    command(data, st, 0, 0xD0);
    CHECK((st->status[0] & 0x30) == 0x30);
    command(data, st, 0, 0x50);
    unlock(data, st, 0);
    command(data, st, 0, 0x40);
    cemu_w30_write16(data, W30_64MBIT_TOP_SIZE, st, 0, 0, 1, NULL);
    CHECK(st->active.kind == W30_OP_PROGRAM);
    command(data, st, 0x10000, 0xB0);
    CHECK(st->active.kind == W30_OP_PROGRAM);
    command(data, st, 0, 0xB0);
    CHECK(st->active.kind == W30_OP_NONE);
    CHECK(st->suspended_program.kind == W30_OP_PROGRAM);
    command(data, st, 0x10000, 0x60);
    CHECK((st->status[0] & 0x30) == 0x30);
    command(data, st, 0x10000, 0xD0);
    CHECK(st->active.kind == W30_OP_PROGRAM);
    cemu_flash_reset(&flash);
    CHECK(st->active.kind == W30_OP_NONE);
    CHECK(read16(data, st, 0, 0) == 0);
    command(data, st, 0, 0x90);
    CHECK(read16(data, st, 4, 1) == 1);
    cemu_flash_state_free(&flash);
    free(data);
}

static void test_protection_and_copy(void) {
    uint8_t *data = malloc(W30_64MBIT_TOP_SIZE);
    CHECK(data != NULL);
    if (!data) return;
    memset(data, 0xFF, W30_64MBIT_TOP_SIZE);
    peripheral_t p;
    w30_state_t st;
    cemu_w30_64mbit_top_periph_init(&p, &st);
    command(data, &st, 0x10A, 0xC0);
    cemu_w30_write16(data, W30_64MBIT_TOP_SIZE, &st, 0x10A,
                0x1234, 1, NULL);
    CHECK(st.active.kind == W30_OP_PROTECTION_PROGRAM);
    cemu_w30_tick(&st, W30_PROGRAM_TICKS);
    command(data, &st, 0, 0x90);
    CHECK(read16(data, &st, 0x10A, 1) == 0x1234);
    command(data, &st, 0x100, 0xC0);
    cemu_w30_write16(data, W30_64MBIT_TOP_SIZE, &st, 0x100,
                0xFFFD, 1, NULL);
    CHECK(!st.customer_locked);
    CHECK(st.active.kind == W30_OP_PROTECTION_PROGRAM);
    cemu_w30_tick(&st, W30_PROGRAM_TICKS - 1u);
    CHECK(!st.customer_locked);
    CHECK(cemu_w30_tick(&st, 1) ==
          W30_TICK_PROTECTION_COMPLETE);
    CHECK(st.customer_locked);
    command(data, &st, W30_PARTITION_SIZE + 0x10A, 0xC0);
    cemu_w30_write16(data, W30_64MBIT_TOP_SIZE, &st,
                W30_PARTITION_SIZE + 0x10A,
                0x0000, 1, NULL);
    CHECK(st.active.kind == W30_OP_NONE);
    CHECK((st.status[1] & 0x30) == 0x10);
    w30_state_t copy = {0};
    CHECK(cemu_w30_state_copy(&copy, &st));
    CHECK(copy.customer_locked && copy.customer[0] == 0x34);
    nor_patch_t invalid[] = {{2, 0}, {1, 0}};
    CHECK(!cemu_w30_state_restore(&copy, &st, invalid, 2));
    cemu_w30_state_free(&copy);
    cemu_w30_state_free(&st);
    free(data);
}

static void test_128mbit_top_descriptor_geometry_ids_and_program(void) {
    const uint32_t size = W30_128MBIT_TOP_SIZE;
    uint8_t *data = malloc(size);
    CHECK(data != NULL);
    if (!data) return;
    memset(data, 0xFF, size);

    peripheral_t p;
    flash_state_t flash;
    CHECK(cemu_flash_periph_init(&p, &flash, W30_128MBIT_TOP_MODEL));
    w30_state_t *st = cemu_flash_w30_state(&flash);
    CHECK(st != NULL);
    CHECK(!strcmp(cemu_flash_model_str(&flash), W30_128MBIT_TOP_MODEL));
    CHECK(st->config->size == size);
    CHECK(st->config->partition_count ==
          W30_128MBIT_TOP_PARTITION_COUNT);
    CHECK(st->config->main_block_count ==
          W30_128MBIT_TOP_MAIN_BLOCK_COUNT);
    CHECK(st->config->block_count == W30_128MBIT_TOP_BLOCK_COUNT);
    CHECK(cemu_w30_block_bitmap_words(st) == W30_MAX_BLOCK_BITMAP_WORDS);

    uint32_t start = 0, block_size = 0;
    CHECK(cemu_w30_block_for_offset(st, 0xFEFFFF, &start, &block_size) == 254);
    CHECK(start == 0xFE0000 && block_size == 0x10000);
    for (unsigned i = 0; i < W30_PARAMETER_BLOCK_COUNT; i++) {
        uint32_t off = W30_128MBIT_TOP_PARAMETER_BASE +
                       i * W30_PARAMETER_BLOCK_SIZE;
        CHECK(cemu_w30_block_for_offset(st, off, &start, &block_size) ==
              (int)(W30_128MBIT_TOP_MAIN_BLOCK_COUNT + i));
        CHECK(start == off && block_size == W30_PARAMETER_BLOCK_SIZE);
    }
    CHECK(cemu_w30_block_for_offset(st, size, NULL, NULL) < 0);

    command(data, st, 0x7D0000, 0x90);
    CHECK(read16(data, st, 0x7D0000, 1) == W30_MANUFACTURER_ID);
    CHECK(read16(data, st, 0x7D0002, 1) ==
          W30_128MBIT_TOP_DEVICE_ID);
    CHECK(read16(data, st, 0x7D0004, 1) == 1);
    CHECK(read16(data, st, 0x7D0100, 1) == 0xFFFE);
    CHECK(read16(data, st, 0x7D010A, 1) == 0xFFFF);
    command(data, st, 0xFF0000, 0x90);
    CHECK(read16(data, st, 0xFF0100, 1) == 0xFFFE);
    CHECK(read16(data, st, 0xFF010A, 1) == 0xFFFF);
    command(data, st, 0x7D0000, 0x98);
    CHECK(read16(data, st, 0x780000 + 0x27 * 2, 1) == 0x0018);
    CHECK(read16(data, st, 0x780000 + 0x2C * 2, 1) == 0x0021);
    CHECK(read16(data, st, 0x780000 + 0x52 * 2, 1) == 0x001F);
    CHECK(read16(data, st, 0x780000 + 0x57 * 2, 1) == 0x001F);

    unlock(data, st, 0x7D0000);
    command(data, st, 0x7D0000, 0x40);
    cemu_w30_write16(data, size, st, 0x7D0000, 0x01FC, 1, NULL);
    CHECK(st->active.kind == W30_OP_PROGRAM);
    CHECK(cemu_w30_tick(st, W30_PROGRAM_TICKS) ==
          W30_TICK_PROGRAM_COMPLETE);
    CHECK(read16(data, st, 0x7D0000, 0) == 0x01FC);

    cemu_flash_state_free(&flash);
    free(data);
}

int main(void) {
    test_model_names();
    test_geometry();
    test_ids_cfi_locks_and_alias();
    test_program_erase_suspend_nested_and_reset();
    test_protection_and_copy();
    test_128mbit_top_descriptor_geometry_ids_and_program();
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
