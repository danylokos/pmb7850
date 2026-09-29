#ifndef EMU_QEMU_AUDIO_H
#define EMU_QEMU_AUDIO_H

#include <gio/gio.h>
#include "emu_engine.h"

typedef struct {
    GDBusConnection *listener;
    GDBusNodeInfo *info;
    guint registration;
    emu_callbacks_t callbacks;
    uint64_t stream;
    uint64_t frames;
    int initialized, enabled, ready, failed;
    char failure[256];
} emu_qemu_audio_t;

void emu_qemu_audio_init(emu_qemu_audio_t *audio,
                         const emu_callbacks_t *callbacks);
emu_error_code_t emu_qemu_audio_attach(emu_qemu_audio_t *audio,
    GDBusConnection *control, emu_error_t *error);
emu_error_code_t emu_qemu_audio_pump(emu_qemu_audio_t *audio,
                                     emu_error_t *error);
/* Same validation/dispatch boundary used by the peer listener and unit tests. */
emu_error_code_t emu_qemu_audio_method(emu_qemu_audio_t *audio,
    const char *method, GVariant *parameters, emu_error_t *error);
void emu_qemu_audio_destroy(emu_qemu_audio_t *audio);

#endif
