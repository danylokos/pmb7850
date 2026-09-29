#include <string.h>

#include "test_support.h"
#include "emu_cemu_adapter.h"

int main(void) {
    const emu_engine_descriptor_t *engine = &emu_cemu_engine_descriptor;
    const emu_engine_diagnostics_t *diagnostics = engine->diagnostics;
    EMU_CHECK(engine && diagnostics);
    EMU_CHECK(!strcmp(engine->name, "cemu"));
    EMU_CHECK(!strcmp(diagnostics->engine_name, "cemu"));
    EMU_CHECK(engine->capabilities ==
              (EMU_CAP_KEYS | EMU_CAP_SERIAL | EMU_CAP_SIM |
               EMU_CAP_BATTERY | EMU_CAP_STORAGE_INIT | EMU_CAP_FRAMES |
               EMU_CAP_EVENTS | EMU_CAP_KEY_SAMPLING |
               EMU_CAP_SERIAL_LINK | EMU_CAP_ARTIFACT_REQ |
               EMU_CAP_AUDIO));
    EMU_CHECK(!(engine->capabilities & EMU_CAP_COVERAGE));
#if CEMU_INSTRUMENTED
    EMU_CHECK(!strcmp(diagnostics->profile_name, "instrumented"));
    EMU_CHECK(diagnostics->capabilities ==
              (EMU_DIAG_MONITOR | EMU_DIAG_ARTIFACTS |
               EMU_DIAG_DEBUGGER | EMU_DIAG_SNAPSHOT |
               EMU_DIAG_COVERAGE | EMU_DIAG_RAW_LCD |
               EMU_DIAG_SNAPSHOT_RESTORE));
    EMU_CHECK(diagnostics->trace_available == CEMU_TRACE_PARQUET);
#else
    EMU_CHECK(!strcmp(diagnostics->profile_name, "plain"));
    EMU_CHECK(diagnostics->capabilities == EMU_DIAG_SNAPSHOT_RESTORE);
    EMU_CHECK(!diagnostics->trace_available);
#endif
    EMU_CHECK(diagnostics->synthetic_parse != NULL);
    puts("CEMU runtime profile descriptor: PASS");
    return 0;
}
