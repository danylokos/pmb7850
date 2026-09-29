#ifndef EMU_DISPATCH_H
#define EMU_DISPATCH_H

#include <stdio.h>

typedef enum {
    EMU_ENTRY_CANONICAL = 0,
    EMU_ENTRY_LEGACY_PLAIN,
    EMU_ENTRY_LEGACY_INSTRUMENTED,
} emu_dispatch_entry_t;

typedef enum {
    EMU_PROFILE_PLAIN = 0,
    EMU_PROFILE_INSTRUMENTED,
} emu_runtime_profile_t;

typedef int (*emu_exec_fn)(const char *, char *const []);
typedef int (*emu_qemu_entry_fn)(int, char **);

int emu_dispatch(emu_dispatch_entry_t entry, int argc, char **argv,
                 const char *plain_runner,
                 const char *instrumented_runner,
                 emu_qemu_entry_fn qemu_entry, emu_exec_fn execute,
                 FILE *out, FILE *err);

#endif
