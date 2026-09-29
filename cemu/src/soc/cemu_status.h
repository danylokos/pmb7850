/* Typed, allocation-free status values for recoverable core operations. */
#ifndef CEMU_STATUS_H
#define CEMU_STATUS_H

#include <stddef.h>

typedef enum {
    CEMU_STATUS_OK = 0,
    CEMU_STATUS_INVALID_ARGUMENT,
    CEMU_STATUS_INVALID_CONFIGURATION,
    CEMU_STATUS_UNSUPPORTED,
    CEMU_STATUS_ALLOCATION_FAILED,
    CEMU_STATUS_TOPOLOGY_FAILED,
    CEMU_STATUS_LIFECYCLE,
    CEMU_STATUS_INITIALIZER_FAILED,
} cemu_status_code_t;

typedef struct {
    cemu_status_code_t code;
    char message[192];
} cemu_status_t;

cemu_status_t cemu_status_ok(void);
cemu_status_t cemu_status_error(cemu_status_code_t code,
                                const char *format, ...);

/* Core allocation wrappers support deterministic construction-failure tests. */
void *cemu_calloc(size_t count, size_t size);
void *cemu_realloc(void *pointer, size_t size);
void cemu_test_fail_alloc_after(long successful_allocations);
void cemu_test_clear_alloc_failure(void);

#endif /* CEMU_STATUS_H */
