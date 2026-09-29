#include <stdio.h>

#include "emu_cemu_adapter.h"
#include "emu_runtime.h"

int main(int argc, char **argv) {
    return emu_cemu_runtime_main(
        argc, argv, &emu_cemu_engine_descriptor);
}
