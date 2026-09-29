#ifndef EMU_QEMU_DISPLAY_H
#define EMU_QEMU_DISPLAY_H

#include <gio/gio.h>

#include "emu_engine.h"
#include "emu_qmp.h"

#define EMU_QEMU_DISPLAY_MAX_WIDTH 130u
#define EMU_QEMU_DISPLAY_MAX_HEIGHT 130u
#define EMU_QEMU_DISPLAY_MAX_RGB_SIZE \
    (EMU_QEMU_DISPLAY_MAX_WIDTH * EMU_QEMU_DISPLAY_MAX_HEIGHT * 3u)

typedef struct {
    GDBusConnection *control;
    GDBusConnection *listener;
    GDBusNodeInfo *listener_info;
    guint registration;
    emu_frame_callback_t callback;
    void *callback_opaque;
    uint8_t pixels[EMU_QEMU_DISPLAY_MAX_WIDTH *
                   EMU_QEMU_DISPLAY_MAX_HEIGHT * 4u];
    uint8_t rgb[EMU_QEMU_DISPLAY_MAX_RGB_SIZE];
    unsigned width;
    unsigned height;
    uint64_t sequence;
    uint64_t icount;
    int enabled;
    int ready;
    int failed;
    char failure[256];
} emu_qemu_display_t;

void emu_qemu_display_init(emu_qemu_display_t *display,
                           emu_frame_callback_t callback, void *opaque,
                           unsigned width, unsigned height);
emu_error_code_t emu_qemu_display_attach(emu_qemu_display_t *display,
                                         emu_qmp_t *qmp,
                                         emu_error_t *error);
emu_error_code_t emu_qemu_display_scanout(
    emu_qemu_display_t *display, unsigned width, unsigned height,
    unsigned stride, unsigned format, const uint8_t *data, size_t size,
    emu_error_t *error);
emu_error_code_t emu_qemu_display_update(
    emu_qemu_display_t *display, int x, int y, int width, int height,
    unsigned stride, unsigned format, const uint8_t *data, size_t size,
    emu_error_t *error);
emu_error_code_t emu_qemu_display_pump(emu_qemu_display_t *display,
                                       emu_error_t *error);
void emu_qemu_display_destroy(emu_qemu_display_t *display);

/* Establish another authenticated listener over the existing display peer. */
GDBusConnection *emu_qemu_peer_listener(
    GDBusConnection *control, const char *path, const char *interface,
    const char *method, GError **error);

#endif
