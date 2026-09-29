#ifndef EMU_TRACE_SCHEMA_H
#define EMU_TRACE_SCHEMA_H

#include <stddef.h>

#include "emu_trace.h"

enum {
    EMU_TRACE_SCHEMA_I64  = 1u << EMU_TRACE_VALUE_I64,
    EMU_TRACE_SCHEMA_BOOL = 1u << EMU_TRACE_VALUE_BOOL,
    EMU_TRACE_SCHEMA_NULL = 1u << EMU_TRACE_VALUE_NULL,
    EMU_TRACE_SCHEMA_STR  = 1u << EMU_TRACE_VALUE_STRING,
};

typedef struct {
    const char *name;
    unsigned types;
} emu_trace_schema_key_t;

typedef struct {
    const char *kind;
    emu_trace_mask_t mask;
    const emu_trace_schema_key_t *keys;
    size_t key_n;
} emu_trace_schema_kind_t;

const emu_trace_schema_kind_t *emu_trace_schema_catalog(size_t *count);
const emu_trace_schema_kind_t *emu_trace_schema_find_kind(const char *kind);
emu_trace_mask_t emu_trace_schema_kind_mask(const char *kind);

int emu_trace_schema_validate_catalog(char *error, size_t cap);
int emu_trace_schema_validate_event(const emu_trace_event_t *event,
                                char *error, size_t cap);

#endif
