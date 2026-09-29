#ifndef EMU_QEMU_TRACE_H
#define EMU_QEMU_TRACE_H

#include <stddef.h>

#include "emu_engine.h"
#include "emu_trace.h"

#define EMU_QEMU_TRACE_MASK \
    (EMU_TRACE_MASK_SERIAL | EMU_TRACE_XBUS_ACCESS | \
     EMU_TRACE_XBUS_MAILBOX_COMPLETE | EMU_TRACE_XBUS_AUDIO_PACKET | EMU_TRACE_MASK_PEC | \
     EMU_TRACE_KEYPAD_COMMAND | EMU_TRACE_KEYPAD_RESULT | \
     EMU_TRACE_KEYPAD_INPUT | EMU_TRACE_KEYPAD_DEFERRED_RELEASE | EMU_TRACE_MASK_TDMA | \
     EMU_TRACE_MASK_SSC0 | EMU_TRACE_MASK_LCD | EMU_TRACE_MASK_LIFECYCLE | \
     EMU_TRACE_MASK_FLASH | EMU_TRACE_MASK_SIM | EMU_TRACE_MASK_BATTERY | \
     EMU_TRACE_MASK_EEPROM | EMU_TRACE_INPUT_OWNER | EMU_TRACE_SERIAL_HISTORY | \
     EMU_TRACE_MASK_FIRMWARE_PATCH)

#define EMU_QEMU_HOST_EVENT_MAX 1536u
int emu_qemu_trace_host_encode(const emu_trace_event_t *, char *, size_t);

int emu_qemu_trace_log_mask(emu_trace_mask_t mask, char *output,
                            size_t capacity);
int emu_qemu_trace_parse_line(emu_trace_sink_t *sink, const char *line,
                              char *error, size_t capacity);
int emu_qemu_trace_validate_simple(const char *path, char *error,
                                   size_t capacity);

#endif
