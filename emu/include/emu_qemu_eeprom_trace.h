#ifndef EMU_QEMU_EEPROM_TRACE_H
#define EMU_QEMU_EEPROM_TRACE_H

#include "emu_qemu_image.h"
#include "emu_trace.h"

typedef struct emu_qemu_eeprom_trace emu_qemu_eeprom_trace_t;

/* Capture prepared bytes before QEMU can mutate its drive files. */
int emu_qemu_eeprom_trace_create(emu_qemu_eeprom_trace_t **out,
    const emu_qemu_image_t *image, char *error, size_t capacity);
int emu_qemu_eeprom_trace_attach(emu_qemu_eeprom_trace_t *trace,
    emu_trace_sink_t *sink, char *error, size_t capacity);
/* 0: unrelated line; 1: consumed physical NOR event; -1: invalid replay. */
int emu_qemu_eeprom_trace_parse(emu_qemu_eeprom_trace_t *trace,
    const char *line, char *error, size_t capacity);
void emu_qemu_eeprom_trace_detach(emu_qemu_eeprom_trace_t *trace);
void emu_qemu_eeprom_trace_destroy(emu_qemu_eeprom_trace_t **trace);

#endif
