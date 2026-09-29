#ifndef EMU_GDB_H
#define EMU_GDB_H

#include <sys/types.h>
#include <termios.h>
#include "emu_runtime.h"

/* The runtime thread owns this supervisor and all engine/transport access. */
typedef struct {
    pid_t child, foreground;
    int terminal, exited, exit_code;
    struct termios original_termios;
    char directory[64];
} emu_gdb_t;

int emu_gdb_prepare(emu_gdb_t *, const emu_cli_options_t *, emu_runtime_options_t *);
int emu_gdb_start(emu_gdb_t *, const emu_runtime_options_t *);
/* 1: exited, 0: alive, -1: host failure. Handles terminal job control. */
int emu_gdb_poll(emu_gdb_t *);
int emu_gdb_interrupt(emu_gdb_t *);
int emu_gdb_stop(emu_gdb_t *);
int emu_gdb_cleanup(emu_gdb_t *);

#endif
