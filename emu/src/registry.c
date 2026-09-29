#include <stdio.h>
#include <string.h>

#include "emu_cemu_adapter.h"
#include "emu_engine.h"

emu_error_code_t emu_registry_init(emu_engine_registry_t *registry,
                                    emu_error_t *error) {
    if (!registry) {
        if (error) {
            error->code = EMU_ERR_ARGUMENT;
            snprintf(error->message, sizeof error->message,
                     "missing registry");
        }
        return EMU_ERR_ARGUMENT;
    }
    memset(registry, 0, sizeof *registry);
    return emu_registry_register(registry, &emu_cemu_engine_descriptor,
                                 error);
}
