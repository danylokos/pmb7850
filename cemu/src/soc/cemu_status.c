#include "cemu_status.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long alloc_fail_after = -1;

cemu_status_t cemu_status_ok(void) {
    cemu_status_t status;
    memset(&status, 0, sizeof(status));
    return status;
}

cemu_status_t cemu_status_error(cemu_status_code_t code,
                                const char *format, ...) {
    cemu_status_t status;
    memset(&status, 0, sizeof(status));
    status.code = code;
    if (format) {
        va_list args;
        va_start(args, format);
        vsnprintf(status.message, sizeof(status.message), format, args);
        va_end(args);
    }
    return status;
}

void *cemu_calloc(size_t count, size_t size) {
    if (alloc_fail_after == 0) return NULL;
    if (alloc_fail_after > 0) alloc_fail_after--;
    return calloc(count, size);
}

void *cemu_realloc(void *pointer, size_t size) {
    if (alloc_fail_after == 0) return NULL;
    if (alloc_fail_after > 0) alloc_fail_after--;
    return realloc(pointer, size);
}

void cemu_test_fail_alloc_after(long successful_allocations) {
    alloc_fail_after = successful_allocations;
}

void cemu_test_clear_alloc_failure(void) {
    alloc_fail_after = -1;
}
