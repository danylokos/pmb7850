#define _POSIX_C_SOURCE 200809L

#include <unistd.h>

#include "emu_dispatch.h"
#include "emu_qemu_adapter.h"
#include "emu_runtime.h"

#ifndef EMU_PLAIN_RUNNER
#define EMU_PLAIN_RUNNER "build/bin/emu-cemu"
#endif

#ifndef EMU_INSTRUMENTED_RUNNER
#define EMU_INSTRUMENTED_RUNNER "build/bin/emu-cemu-inst"
#endif

#ifndef EMU_DISPATCH_ENTRY
#define EMU_DISPATCH_ENTRY EMU_ENTRY_CANONICAL
#endif

#ifndef EMU_QEMU_BINARY
#define EMU_QEMU_BINARY "qemu-system-c166"
#endif

static int qemu_main(int argc, char **argv) {
    return emu_qemu_runtime_main(argc, argv, &emu_qemu_engine_descriptor,
                                 EMU_QEMU_BINARY);
}

int main(int argc, char **argv) {
    return emu_dispatch(EMU_DISPATCH_ENTRY, argc, argv,
                        EMU_PLAIN_RUNNER, EMU_INSTRUMENTED_RUNNER,
                        qemu_main, execv, stdout, stderr);
}
