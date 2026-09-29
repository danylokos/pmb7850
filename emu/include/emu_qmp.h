#ifndef EMU_QMP_H
#define EMU_QMP_H

#include "emu_engine.h"

#define EMU_QMP_MESSAGE_MAX 65536u
#define EMU_QMP_NESTING_MAX 64u
#define EMU_QMP_VERSION_MAX 128u

typedef struct {
    int fd;
    uint64_t next_id;
    char input[EMU_QMP_MESSAGE_MAX];
    size_t input_size;
    char version[EMU_QMP_VERSION_MAX];
    char build[EMU_QMP_VERSION_MAX];
} emu_qmp_t;

void emu_qmp_init(emu_qmp_t *qmp, int fd);
emu_error_code_t emu_qmp_handshake(emu_qmp_t *qmp, unsigned timeout_ms,
                                   emu_error_t *error);
emu_error_code_t emu_qmp_command(emu_qmp_t *qmp, const char *command,
                                 const char *arguments_json, int send_fd,
                                 char *response, size_t response_capacity,
                                 unsigned timeout_ms, emu_error_t *error);
int emu_qmp_next_message(emu_qmp_t *qmp, char *message, size_t capacity,
                         unsigned timeout_ms, emu_error_t *error);

#endif
