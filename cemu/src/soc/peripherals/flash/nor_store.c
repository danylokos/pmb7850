#include "nor_store.h"

#include <stdlib.h>
#include <string.h>

static int reserve(nor_store_t *store, size_t need) {
    if (store->cap >= need) return 1;
    size_t cap = store->cap ? store->cap : 8u;
    while (cap < need) {
        if (cap > SIZE_MAX / 2u) return 0;
        cap *= 2u;
    }
    void *p = realloc(store->patches, cap * sizeof(*store->patches));
    if (!p) return 0;
    store->patches = p;
    store->cap = cap;
    return 1;
}

static int lookup(const nor_store_t *store, uint32_t off, size_t *index_out) {
    size_t lo = 0, hi = store ? store->count : 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        uint32_t current = store->patches[mid].off;
        if (current == off) {
            if (index_out) *index_out = mid;
            return 1;
        }
        if (current < off) lo = mid + 1u;
        else hi = mid;
    }
    if (index_out) *index_out = lo;
    return 0;
}

void cemu_nor_store_free(nor_store_t *store) {
    if (!store) return;
    free(store->patches);
    memset(store, 0, sizeof(*store));
}

int cemu_nor_store_validate(const nor_patch_t *patches, size_t count,
                       uint32_t capacity) {
    if ((count && !patches) || count > capacity) return 0;
    for (size_t i = 0; i < count; i++)
        if (patches[i].off >= capacity ||
            (i && patches[i - 1u].off >= patches[i].off))
            return 0;
    return 1;
}

int cemu_nor_store_restore(nor_store_t *store, const nor_patch_t *patches,
                      size_t count, uint32_t capacity) {
    if (!store || !cemu_nor_store_validate(patches, count, capacity)) return 0;
    nor_store_t replacement = {0};
    if (count) {
        replacement.patches = malloc(count * sizeof(*replacement.patches));
        if (!replacement.patches) return 0;
        memcpy(replacement.patches, patches,
               count * sizeof(*replacement.patches));
        replacement.count = count;
        replacement.cap = count;
    }
    cemu_nor_store_free(store);
    *store = replacement;
    return 1;
}

int cemu_nor_store_copy(nor_store_t *dst, const nor_store_t *src,
                   uint32_t capacity) {
    if (!dst || !src) return 0;
    return cemu_nor_store_restore(dst, src->patches, src->count, capacity);
}

int cemu_nor_store_seed(nor_store_t *store, uint32_t capacity, uint32_t off,
                   const uint8_t *bytes, size_t count) {
    if (!store || (!bytes && count) || off > capacity ||
        count > capacity - off || count > UINT32_MAX - off ||
        !reserve(store, store->count + count))
        return 0;
    for (size_t i = 0; i < count; i++) {
        size_t index;
        uint32_t byte_off = off + (uint32_t)i;
        if (lookup(store, byte_off, &index)) {
            store->patches[index].value = bytes[i];
            continue;
        }
        memmove(&store->patches[index + 1u], &store->patches[index],
                (store->count - index) * sizeof(*store->patches));
        store->patches[index] = (nor_patch_t){byte_off, bytes[i]};
        store->count++;
    }
    return 1;
}

uint8_t cemu_nor_store_read(const nor_store_t *store, const uint8_t *backing,
                       size_t backing_len, uint32_t off, int backing_erased) {
    size_t index;
    if (!store || off >= backing_len) return 0xFF;
    if (lookup(store, off, &index)) return store->patches[index].value;
    return backing_erased ? 0xFF : backing[off];
}

int cemu_nor_store_program_byte(nor_store_t *store, const uint8_t *backing,
                           size_t backing_len, uint32_t capacity,
                           uint32_t off, uint8_t requested,
                           int backing_erased, int *program_error) {
    if (!store || !backing || off >= backing_len || off >= capacity) return 0;
    uint8_t old = cemu_nor_store_read(store, backing, backing_len, off,
                                 backing_erased);
    uint8_t programmed = old & requested;
    if (program_error && programmed != requested) *program_error = 1;
    return cemu_nor_store_seed(store, capacity, off, &programmed, 1);
}

void cemu_nor_store_erase_range(nor_store_t *store, uint32_t start, uint32_t end) {
    if (!store || start >= end) return;
    size_t first;
    (void)lookup(store, start, &first);
    size_t last;
    (void)lookup(store, end, &last);
    if (last > first) {
        memmove(&store->patches[first], &store->patches[last],
                (store->count - last) * sizeof(*store->patches));
        store->count -= last - first;
    }
}

const nor_patch_t *cemu_nor_store_patches(const nor_store_t *store,
                                     size_t *count_out) {
    if (count_out) *count_out = store ? store->count : 0;
    return store ? store->patches : NULL;
}
