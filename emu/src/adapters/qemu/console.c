#include "emu_qemu_console.h"

#include <string.h>

static const char *decimal(const char *p) {
    if (*p < '0' || *p > '9') return NULL;
    while (*p >= '0' && *p <= '9') p++;
    if (*p++ != '.') return NULL;
    if (*p < '0' || *p > '9') return NULL;
    return p + 1;
}

static int delay_warning(const char *line) {
    static const char prefix[] = "Warning: The guest is now late by ";
    if (strncmp(line, prefix, sizeof prefix - 1)) return 0;
    const char *p = decimal(line + sizeof prefix - 1);
    if (!p || strncmp(p, " to ", 4)) return 0;
    p = decimal(p + 4);
    return p && (!strcmp(p, " seconds\n") || !strcmp(p, " seconds"));
}

static void emit(emu_qemu_console_t *s, emu_console_callback_t callback,
                  void *opaque) {
    s->line[s->size] = 0;
    int warning = !s->ordinary && strlen(s->line) == s->size &&
                  delay_warning(s->line);
    if (s->size) callback(opaque, warning ? EMU_CONSOLE_DELAY_WARNING
                                          : EMU_CONSOLE_OUTPUT,
                          s->line, s->size);
    s->size = 0;
}

void emu_qemu_console_feed(emu_qemu_console_t *s,
                           emu_console_callback_t callback, void *opaque,
                           const char *bytes, size_t size, int eof) {
    for (size_t i = 0; i < size; i++) {
        if (s->size == sizeof s->line - 1) {
            s->ordinary = 1;
            emit(s, callback, opaque);
        }
        s->line[s->size++] = bytes[i];
        if (bytes[i] == '\n') {
            emit(s, callback, opaque);
            s->ordinary = 0;
        }
    }
    if (eof) {
        emit(s, callback, opaque);
        s->ordinary = 0;
    }
}
