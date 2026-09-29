/* Model-neutral metadata describing one externally visible flash access. */
#ifndef CEMU_PERIPH_FLASH_ACCESS_H
#define CEMU_PERIPH_FLASH_ACCESS_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    FLASH_MUTATION_PROGRAM,
    FLASH_MUTATION_ERASE,
} flash_mutation_kind_t;

typedef struct {
    flash_mutation_kind_t kind;
    uint32_t offset;
    uint32_t size;
} flash_mutation_t;

#define FLASH_ACCESS_MAX_MUTATIONS 16u

typedef struct {
    const char *subtype;
    const char *detail;
    int include_mode;
    size_t mutation_count;
    flash_mutation_t mutations[FLASH_ACCESS_MAX_MUTATIONS];
} flash_access_t;

static inline void flash_access_add_mutation(
        flash_access_t *access, flash_mutation_kind_t kind,
        uint32_t offset, uint32_t size) {
    if (!access || !size ||
        access->mutation_count >= FLASH_ACCESS_MAX_MUTATIONS)
        return;
    access->mutations[access->mutation_count++] =
        (flash_mutation_t){kind, offset, size};
}

#endif
