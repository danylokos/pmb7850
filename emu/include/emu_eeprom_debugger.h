#ifndef EMU_EEPROM_DEBUGGER_H
#define EMU_EEPROM_DEBUGGER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "emu_eeprom_trace.h"

typedef struct emu_eeprom_debugger emu_eeprom_debugger_t;

typedef void (*emu_eeprom_debugger_stop_fn)(
    void *opaque, const char *kind, uint32_t pc, uint64_t icount,
    const char *reason);

int emu_eeprom_debugger_create(
    emu_eeprom_debugger_t **out,
    const emu_eeprom_trace_chip_t *chips, size_t chip_count,
    emu_eeprom_trace_copy_fn copy, void *copy_opaque,
    emu_eeprom_debugger_stop_fn stop, void *stop_opaque,
    uint64_t tick, uint64_t icount, uint32_t pc);
void emu_eeprom_debugger_destroy(emu_eeprom_debugger_t **debugger);
void emu_eeprom_debugger_resync(
    emu_eeprom_debugger_t *debugger,
    uint64_t tick, uint64_t icount, uint32_t pc);
void emu_eeprom_debugger_help(
    emu_eeprom_debugger_t *debugger, FILE *out);

/* Return nonzero when argv is an EEPROM debugger command. */
int emu_eeprom_debugger_command(
    emu_eeprom_debugger_t *debugger, int argc,
    char *const *argv, FILE *out);
void emu_eeprom_debugger_help(
    emu_eeprom_debugger_t *debugger, FILE *out);
/* Re-read restored flash without resetting watches or accumulated statistics. */
void emu_eeprom_debugger_resync(
    emu_eeprom_debugger_t *debugger,
    uint64_t tick, uint64_t icount, uint32_t pc);

void emu_eeprom_debugger_read(
    emu_eeprom_debugger_t *debugger, int chip_index,
    uint32_t chip_offset, uint32_t guest_addr, uint32_t value, size_t size,
    uint64_t tick, uint64_t icount, uint32_t pc);
void emu_eeprom_debugger_mutation(
    emu_eeprom_debugger_t *debugger, int chip_index,
    emu_eeprom_trace_mutation_kind_t kind, uint32_t offset, uint32_t size,
    uint64_t tick, uint64_t icount, uint32_t pc);

#endif
