#include <stdio.h>
#include <string.h>

#include "nor_store.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    uint8_t backing[16];
    memset(backing, 0xA5, sizeof backing);
    nor_store_t store = {0};
    const uint8_t seed[] = {0x12, 0x34, 0x56};
    CHECK(cemu_nor_store_seed(&store, sizeof backing, 4, seed, sizeof seed));
    CHECK(store.count == 3);
    CHECK(store.patches[0].off == 4 && store.patches[2].off == 6);
    const uint8_t replacement = 0xFF;
    CHECK(cemu_nor_store_seed(&store, sizeof backing, 5, &replacement, 1));
    CHECK(store.count == 3 && store.patches[1].value == 0xFF);
    CHECK(cemu_nor_store_read(&store, backing, sizeof backing, 5, 0) == 0xFF);
    CHECK(cemu_nor_store_read(&store, backing, sizeof backing, 8, 0) == 0xA5);
    CHECK(cemu_nor_store_read(&store, backing, sizeof backing, 8, 1) == 0xFF);

    int error = 0;
    CHECK(cemu_nor_store_program_byte(&store, backing, sizeof backing,
                                 sizeof backing, 5, 0x0F, 0, &error));
    CHECK(!error && cemu_nor_store_read(&store, backing, sizeof backing, 5, 0) ==
                    0x0F);
    CHECK(cemu_nor_store_program_byte(&store, backing, sizeof backing,
                                 sizeof backing, 5, 0xFF, 0, &error));
    CHECK(error && cemu_nor_store_read(&store, backing, sizeof backing, 5, 0) ==
                   0x0F);

    nor_store_t copy = {0};
    CHECK(cemu_nor_store_copy(&copy, &store, sizeof backing));
    cemu_nor_store_erase_range(&copy, 4, 6);
    CHECK(copy.count == 1 && copy.patches[0].off == 6);
    CHECK(store.count == 3);
    nor_patch_t invalid[] = {{2, 1}, {1, 2}};
    CHECK(!cemu_nor_store_restore(&copy, invalid, 2, sizeof backing));
    CHECK(copy.count == 1);
    cemu_nor_store_free(&copy);
    cemu_nor_store_free(&store);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
