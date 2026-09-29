#ifndef EMU_TEST_FAKE_ENGINE_H
#define EMU_TEST_FAKE_ENGINE_H
#include "emu_engine.h"
extern const emu_engine_descriptor_t emu_fake_engine_descriptor;
int emu_fake_live_sessions(void);
void emu_fake_fail_next_create(void);
void emu_fake_fail_next_artifact(void);
#endif
