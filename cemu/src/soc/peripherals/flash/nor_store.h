/* Sparse mutable byte storage shared by NOR command engines. */
#ifndef CEMU_PERIPH_NOR_STORE_H
#define CEMU_PERIPH_NOR_STORE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t off;
    uint8_t value;
} nor_patch_t;

typedef struct {
    nor_patch_t *patches;
    size_t count;
    size_t cap;
} nor_store_t;

void cemu_nor_store_free(nor_store_t *store);
int cemu_nor_store_validate(const nor_patch_t *patches, size_t count,
                       uint32_t capacity);
int cemu_nor_store_restore(nor_store_t *store, const nor_patch_t *patches,
                      size_t count, uint32_t capacity);
int cemu_nor_store_copy(nor_store_t *dst, const nor_store_t *src,
                   uint32_t capacity);
int cemu_nor_store_seed(nor_store_t *store, uint32_t capacity, uint32_t off,
                   const uint8_t *bytes, size_t count);
uint8_t cemu_nor_store_read(const nor_store_t *store, const uint8_t *backing,
                       size_t backing_len, uint32_t off, int backing_erased);
int cemu_nor_store_program_byte(nor_store_t *store, const uint8_t *backing,
                           size_t backing_len, uint32_t capacity,
                           uint32_t off, uint8_t requested,
                           int backing_erased, int *program_error);
void cemu_nor_store_erase_range(nor_store_t *store, uint32_t start, uint32_t end);
const nor_patch_t *cemu_nor_store_patches(const nor_store_t *store,
                                     size_t *count_out);

#endif
