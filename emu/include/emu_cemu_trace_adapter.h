#ifndef EMU_CEMU_TRACE_ADAPTER_H
#define EMU_CEMU_TRACE_ADAPTER_H

#include <stddef.h>

#include "emu_trace.h"

typedef struct cemu_native_trace_event cemu_native_trace_event_t;
typedef struct cemu_core cemu_core_t;
typedef struct emu_cemu_trace_adapter emu_cemu_trace_adapter_t;

void emu_cemu_trace_emit(
    emu_trace_sink_t *sink, const cemu_native_trace_event_t *event);
int emu_cemu_trace_validate_event(
    const cemu_native_trace_event_t *event, char *error, size_t capacity);
int emu_cemu_trace_attach(
    cemu_core_t *core, emu_trace_sink_t *sink,
    emu_cemu_trace_adapter_t **adapter);
int emu_cemu_trace_attach_deferred(
    cemu_core_t *core, emu_trace_sink_t *sink, int deferred,
    emu_cemu_trace_adapter_t **adapter);
int emu_cemu_trace_set_enabled(emu_cemu_trace_adapter_t *adapter,
                               int enabled);
void emu_cemu_trace_detach(emu_cemu_trace_adapter_t **adapter);

#endif
