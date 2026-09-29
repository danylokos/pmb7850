#ifndef EMU_QEMU_CONSOLE_H
#define EMU_QEMU_CONSOLE_H

#include "emu_engine.h"

/* Bounded line recognition; oversized lines stream unchanged as ordinary output. */
typedef struct {
    char line[256];
    size_t size;
    int ordinary;
} emu_qemu_console_t;
void emu_qemu_console_feed(emu_qemu_console_t *, emu_console_callback_t,
                           void *, const char *, size_t, int eof);

#endif
