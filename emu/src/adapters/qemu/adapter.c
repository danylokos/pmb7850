#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "emu_qemu_adapter.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "emu_product.h"
#include "emu_patch.h"
#include "emu_qemu_audio.h"
#include "emu_qemu_display.h"
#include "emu_qemu_eeprom_trace.h"
#include "emu_qemu_image.h"
#include "emu_qemu_snapshot.h"
#include "emu_qmp.h"
#include "emu_qemu_console.h"
#include "emu_qemu_trace.h"

extern char **environ;

typedef struct {
    emu_qemu_image_t image;
    const emu_prepared_session_t *prepared;
    emu_qemu_snapshot_t snapshot;
    char snapshot_path[EMU_QEMU_PATH_MAX];
    int snapshot_requested;
    int restored;
    uint64_t initial_ticks, initial_instructions;
    emu_qmp_t qmp;
    emu_qemu_display_t display;
    emu_qemu_audio_t audio;
    emu_callbacks_t callbacks;
    const emu_product_t *product;
    pid_t child;
    int asc_fd;
    int stderr_fd;
    int stdout_fd;
    emu_qemu_console_t console;
    int alive;
    int started;
    int stop_requested;
    int exit_status;
    uint32_t ui_buttons;
    uint32_t sampled_buttons;
    emu_serial_link_attachment_t serial_attachment;
    uint64_t serial_rx_bytes;
    uint64_t serial_tx_bytes;
    uint64_t last_stats_ns;
    emu_engine_state_t state;
    char stderr_text[2048];
    size_t stderr_size;
    emu_trace_mask_t trace_mask;
    emu_trace_sink_t *trace_sink;
    emu_qemu_eeprom_trace_t *eeprom_trace;
    FILE *host_trace_tail;
    char native_trace_path[EMU_QEMU_PATH_MAX];
    char trace_line[4096];
    size_t trace_line_size;
    int trace_failed;
    int sim_attached;
    unsigned gdb_port;
    int gdb_enabled;
    char gdb_socket[108];
    char trace_error[256];
} emu_qemu_session_t;

static void read_stderr(emu_qemu_session_t *session);

static const char *const qemu_models[] = {
    "a52", "a55", "a60", "a62", "a65", "c55",
    "c60", "cf62", "m55", "mc60", "s55", "sl55",
};
static char qemu_version[EMU_QMP_VERSION_MAX] = "unknown";

static emu_error_code_t qemu_fail(emu_error_t *error,
                                  emu_error_code_t code,
                                  const char *format, ...) {
    if (error) {
        va_list arguments;
        error->code = code;
        va_start(arguments, format);
        vsnprintf(error->message, sizeof error->message, format, arguments);
        va_end(arguments);
    }
    return code;
}

static void qemu_ok(emu_error_t *error) {
    if (error) {
        error->code = EMU_OK;
        error->message[0] = 0;
    }
}

static uint64_t monotonic_ns(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) return 0;
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) +
           (uint64_t)time.tv_nsec;
}

static int promoted_fd(int fd) {
    int promoted = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    int saved = errno;
    close(fd);
    errno = saved;
    return promoted;
}

static int make_socket_pair(int sockets[2]) {
    int original[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, original)) return -1;
    sockets[0] = promoted_fd(original[0]);
    sockets[1] = promoted_fd(original[1]);
    if (sockets[0] < 0 || sockets[1] < 0) {
        if (sockets[0] >= 0) close(sockets[0]);
        if (sockets[1] >= 0) close(sockets[1]);
        return -1;
    }
    return 0;
}

static int make_pipe(int pipes[2]) {
    int original[2];
    if (pipe(original)) return -1;
    pipes[0] = promoted_fd(original[0]);
    pipes[1] = promoted_fd(original[1]);
    if (pipes[0] < 0 || pipes[1] < 0) {
        if (pipes[0] >= 0) close(pipes[0]);
        if (pipes[1] >= 0) close(pipes[1]);
        return -1;
    }
    return 0;
}

static int nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}

static int valid_machine_device_name(const char *name) {
    if (!name || !name[0]) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '-'))
            return 0;
    return 1;
}

static const char *qemu_flash_type(const emu_qemu_chip_image_t *chip) {
    if (!strcmp(chip->model, "m58lw064d") && chip->size == 4u * 1024u * 1024u)
        return "m58-compatible-32mbit";
    return chip->model;
}

static int hex_bytes(char *output, size_t output_size,
                     const uint8_t *bytes, size_t size) {
    if (!output || output_size < 2u * size + 1u) return -1;
    for (size_t i = 0; i < size; i++)
        snprintf(output + 2u * i, 3u, "%02X", bytes[i]);
    return 0;
}

static emu_error_code_t spawn_qemu(emu_qemu_session_t *session,
                                   const char *binary,
                                   emu_error_t *error) {
    int qmp_fds[2] = {-1, -1};
    int asc_fds[2] = {-1, -1};
    int err_fds[2] = {-1, -1};
    int out_fds[2] = {-1, -1};
    if (make_socket_pair(qmp_fds) || make_socket_pair(asc_fds) ||
        make_pipe(err_fds) ||
        (session->callbacks.console && make_pipe(out_fds)))
        goto system_fail;
    int highest_fd = qmp_fds[0];
    int *all_fds[] = {&qmp_fds[1], &asc_fds[0], &asc_fds[1],
                      &err_fds[0], &err_fds[1], &out_fds[0], &out_fds[1]};
    for (size_t i = 0; i < sizeof all_fds / sizeof all_fds[0]; i++)
        if (*all_fds[i] > highest_fd) highest_fd = *all_fds[i];
    int qmp_child_fd = highest_fd + 1;
    int asc_child_fd = highest_fd + 2;
    char machine[192];
    char drives[EMU_MAX_CHIPS][EMU_QEMU_PATH_MAX + 64u];
    char identities[EMU_MAX_CHIPS][640];
    char qmp_chardev[64];
    char asc_chardev[64];
    char gdb_endpoint[160];
    char trace_log_mask[2048];
    if (!session->product ||
        strcmp(session->product->name, session->image.device) ||
        !valid_machine_device_name(session->product->name) ||
        session->image.chip_count != session->product->chip_count) {
        qemu_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                  "invalid QEMU machine product");
        goto fail;
    }
    int machine_size;
    if (session->image.chip_count == 2) {
        machine_size = snprintf(machine, sizeof machine,
            "siemens-%s,audiodev=emu-audio,flash-type=%s,secondary-flash-type=%s",
            session->product->name,
            qemu_flash_type(&session->image.chips[0]),
            qemu_flash_type(&session->image.chips[1]));
    } else {
        machine_size = snprintf(machine, sizeof machine,
            "siemens-%s,audiodev=emu-audio,flash-type=%s", session->product->name,
            qemu_flash_type(&session->image.chips[0]));
    }
    int drives_valid = 1;
    for (size_t i = 0; i < session->image.chip_count; i++) {
        int drive_size = snprintf(
            drives[i], sizeof drives[i],
            "if=pflash,index=%zu,format=raw,file=%s,id=flash%zu", i,
            session->image.chips[i].path, i);
        if (drive_size < 0 || (size_t)drive_size >= sizeof drives[i])
            drives_valid = 0;
    }
    int identities_valid = 1;
    size_t identity_count = 0;
    for (size_t i = 0; i < session->image.chip_count; i++) {
        const emu_qemu_chip_image_t *chip = &session->image.chips[i];
        const char *type = qemu_flash_type(chip);
        char value[2u * EMU_QEMU_AM29_SECSI_SIZE + 1u];
        int size = 0;

        if (chip->factory_uid_set) {
            if (hex_bytes(value, sizeof value, chip->factory_uid,
                          sizeof chip->factory_uid)) {
                identities_valid = 0;
                continue;
            }
            size = snprintf(identities[identity_count],
                            sizeof identities[identity_count],
                            "%s.factory-uid=%s", type, value);
        } else if (chip->am29_secsi_set) {
            if (hex_bytes(value, sizeof value, chip->am29_secsi,
                          sizeof chip->am29_secsi)) {
                identities_valid = 0;
                continue;
            }
            size = snprintf(identities[identity_count],
                            sizeof identities[identity_count],
                            "%s.factory-secsi=%s", type, value);
        } else if (i == session->product->identity_chip_index) {
            qemu_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                      "prepared %s image has no immutable flash identity",
                      session->product->name);
            goto fail;
        } else {
            continue;
        }
        if (size < 0 || (size_t)size >= sizeof identities[identity_count])
            identities_valid = 0;
        identity_count++;
    }
    int qmp_size = snprintf(qmp_chardev, sizeof qmp_chardev,
                            "socket,id=qmp,fd=%d", qmp_child_fd);
    int asc_size = snprintf(asc_chardev, sizeof asc_chardev,
                            "socket,id=asc0,fd=%d", asc_child_fd);
    int gdb_size = session->gdb_enabled
        ? (session->gdb_socket[0]
           ? snprintf(gdb_endpoint, sizeof gdb_endpoint, "unix:%s,server=on,wait=off", session->gdb_socket)
           : snprintf(gdb_endpoint, sizeof gdb_endpoint,
                      "tcp:127.0.0.1:%u", session->gdb_port))
        : 0;
    if (machine_size < 0 || (size_t)machine_size >= sizeof machine ||
        !drives_valid ||
        !identities_valid ||
        qmp_size < 0 || (size_t)qmp_size >= sizeof qmp_chardev ||
        asc_size < 0 || (size_t)asc_size >= sizeof asc_chardev ||
        gdb_size < 0 || (size_t)gdb_size >= sizeof gdb_endpoint) {
        qemu_fail(error, EMU_ERR_ARGUMENT, "QEMU launch argument is too long");
        goto fail;
    }
    if ((session->trace_mask & ~EMU_TRACE_MASK_FIRMWARE_PATCH) &&
        emu_qemu_trace_log_mask(session->trace_mask, trace_log_mask,
                                sizeof trace_log_mask)) {
        qemu_fail(error, EMU_ERR_ARGUMENT, "QEMU trace selection is too long");
        goto fail;
    }
    char *arguments[64];
    size_t argument_count = 0;
#define ARG(value) arguments[argument_count++] = (char *)(value)
    ARG(binary);
    ARG("-M"); ARG(machine);
    for (size_t i = 0; i < session->image.chip_count; i++) {
        ARG("-drive"); ARG(drives[i]);
    }
    for (size_t i = 0; i < identity_count; i++) {
        ARG("-global"); ARG(identities[i]);
    }
    ARG("-accel"); ARG("tcg,thread=single");
    ARG("-icount"); ARG("shift=4,align=on,sleep=on");
    if (session->trace_mask & EMU_TRACE_EEPROM_ACCESS) {
        ARG("-global"); ARG("pmb7850-soc.observe-flash-reads=on");
    }
    ARG("-display"); ARG("dbus,p2p=on,gl=off,audiodev=emu-audio");
    ARG("-audiodev"); ARG("dbus,id=emu-audio,direct-pcm=on");
    ARG("-chardev"); ARG(qmp_chardev);
    ARG("-object"); ARG("monitor-qmp,id=qmp0,chardev=qmp");
    ARG("-chardev"); ARG(asc_chardev);
    ARG("-serial"); ARG("chardev:asc0");
    if (session->sim_attached) {
        ARG("-global"); ARG("pmb7850-soc.sim-profile=deterministic");
    }
    if (session->native_trace_path[0]) {
        ARG("-d"); ARG(trace_log_mask);
        ARG("-D"); ARG(session->native_trace_path);
    }
    if (session->gdb_enabled) {
        ARG("-gdb"); ARG(gdb_endpoint);
    }
    /* Restore remains paused, including snapshots taken at a GDB stop. */
    ARG("-global"); ARG("migration.store-global-state=off");
    if (session->snapshot_path[0]) {
        ARG("-incoming"); ARG("defer");
    }
    ARG("-S"); ARG("-no-reboot"); ARG("-no-shutdown");
    arguments[argument_count] = NULL;
#undef ARG
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) goto system_fail;
    int action_failed =
        posix_spawn_file_actions_adddup2(&actions, qmp_fds[1], qmp_child_fd) ||
        posix_spawn_file_actions_adddup2(&actions, asc_fds[1], asc_child_fd) ||
        posix_spawn_file_actions_adddup2(&actions, err_fds[1], STDERR_FILENO) ||
        posix_spawn_file_actions_addclose(&actions, qmp_fds[0]) ||
        posix_spawn_file_actions_addclose(&actions, qmp_fds[1]) ||
        posix_spawn_file_actions_addclose(&actions, asc_fds[0]) ||
        posix_spawn_file_actions_addclose(&actions, asc_fds[1]) ||
        posix_spawn_file_actions_addclose(&actions, err_fds[0]) ||
        posix_spawn_file_actions_addclose(&actions, err_fds[1]);
    if (!action_failed && out_fds[0] >= 0)
        action_failed =
            posix_spawn_file_actions_adddup2(&actions, out_fds[1], STDOUT_FILENO) ||
            posix_spawn_file_actions_addclose(&actions, out_fds[0]) ||
            posix_spawn_file_actions_addclose(&actions, out_fds[1]);
    if (action_failed) {
        posix_spawn_file_actions_destroy(&actions);
        goto system_fail;
    }
    posix_spawnattr_t attributes;
    int attribute_status = posix_spawnattr_init(&attributes);
    int attributes_initialized = attribute_status == 0;
    if (!attribute_status)
        attribute_status = posix_spawnattr_setflags(
            &attributes, POSIX_SPAWN_SETPGROUP);
    if (!attribute_status)
        attribute_status = posix_spawnattr_setpgroup(&attributes, 0);
    if (attribute_status) {
        if (attributes_initialized) posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        errno = attribute_status;
        goto system_fail;
    }
    int status = posix_spawn(&session->child, binary, &actions, &attributes,
                             arguments, environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (status) {
        errno = status;
        goto system_fail;
    }
    close(qmp_fds[1]);
    close(asc_fds[1]);
    close(err_fds[1]);
    if (out_fds[1] >= 0) close(out_fds[1]);
    session->stdout_fd = out_fds[0];
    session->asc_fd = asc_fds[0];
    session->stderr_fd = err_fds[0];
    session->alive = 1;
    emu_qmp_init(&session->qmp, qmp_fds[0]);
    if (nonblocking(session->asc_fd) || nonblocking(session->stderr_fd) ||
        (session->stdout_fd >= 0 && nonblocking(session->stdout_fd)))
        return qemu_fail(error, EMU_ERR_IO,
                         "cannot configure QEMU transport descriptors");
    qemu_ok(error);
    return EMU_OK;

system_fail:
    qemu_fail(error, EMU_ERR_IO, "cannot spawn QEMU: %s", strerror(errno));
fail:
    for (size_t i = 0; i < 2; i++) {
        if (qmp_fds[i] >= 0) close(qmp_fds[i]);
        if (asc_fds[i] >= 0) close(asc_fds[i]);
        if (err_fds[i] >= 0) close(err_fds[i]);
        if (out_fds[i] >= 0) close(out_fds[i]);
    }
    return error ? error->code : EMU_ERR_IO;
}

static void read_stdout(emu_qemu_session_t *session) {
    if (session->stdout_fd < 0) return;
    char bytes[4096];
    for (;;) {
        ssize_t count = read(session->stdout_fd, bytes, sizeof bytes);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        emu_qemu_console_feed(&session->console, session->callbacks.console,
                              session->callbacks.opaque, bytes,
                              count > 0 ? (size_t)count : 0, count <= 0);
        if (count <= 0) {
            close(session->stdout_fd);
            session->stdout_fd = -1;
            return;
        }
    }
}

static emu_error_code_t qom_get_u64(emu_qemu_session_t *session,
                                    const char *path, const char *property,
                                    uint64_t *value, emu_error_t *error) {
    char arguments[256];
    char response[1024];
    int length = snprintf(arguments, sizeof arguments,
        "{\"path\":\"%s\",\"property\":\"%s\"}", path, property);
    if (length < 0 || (size_t)length >= sizeof arguments)
        return qemu_fail(error, EMU_ERR_ARGUMENT, "invalid QOM property");
    emu_error_code_t code = emu_qmp_command(
        &session->qmp, "qom-get", arguments, -1, response,
        sizeof response, 5000, error);
    if (code != EMU_OK) return code;
    const char *cursor = strstr(response, "\"return\"");
    if (!cursor || !(cursor = strchr(cursor, ':')))
        return qemu_fail(error, EMU_ERR_ENGINE, "invalid QOM integer reply");
    cursor++;
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (*cursor < '0' || *cursor > '9')
        return qemu_fail(error, EMU_ERR_ENGINE, "invalid QOM integer value");
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(cursor, &end, 10);
    if (errno || end == cursor)
        return qemu_fail(error, EMU_ERR_ENGINE, "invalid QOM integer value");
    *value = (uint64_t)parsed;
    qemu_ok(error);
    return EMU_OK;
}

static emu_error_code_t qom_set_u64(emu_qemu_session_t *session,
                                    const char *path, const char *property,
                                    uint64_t value, emu_error_t *error) {
    char arguments[256];
    int length = snprintf(arguments, sizeof arguments,
        "{\"path\":\"%s\",\"property\":\"%s\",\"value\":%llu}",
        path, property, (unsigned long long)value);
    if (length < 0 || (size_t)length >= sizeof arguments)
        return qemu_fail(error, EMU_ERR_ARGUMENT, "invalid QOM property");
    return emu_qmp_command(&session->qmp, "qom-set", arguments, -1,
                           NULL, 0, 5000, error);
}

static emu_error_code_t qom_set_bool(emu_qemu_session_t *session,
                                     const char *path, const char *property,
                                     int value, emu_error_t *error) {
    char arguments[256];
    int length = snprintf(arguments, sizeof arguments,
        "{\"path\":\"%s\",\"property\":\"%s\",\"value\":%s}",
        path, property, value ? "true" : "false");
    if (length < 0 || (size_t)length >= sizeof arguments)
        return qemu_fail(error, EMU_ERR_ARGUMENT, "invalid QOM property");
    return emu_qmp_command(&session->qmp, "qom-set", arguments, -1,
                           NULL, 0, 5000, error);
}

static emu_error_code_t update_stats(emu_qemu_session_t *session,
                                     emu_error_t *error) {
    emu_engine_state_t observed = session->state;
    uint64_t value;
    emu_error_code_t code = qom_get_u64(
        session, "/machine", "guest-instructions", &value, error);
    if (code != EMU_OK) return code;
    observed.guest_instructions = value;
    observed.icount = value;
    code = qom_get_u64(session, "/machine", "timing-ticks", &value, error);
    if (code != EMU_OK) return code;
    observed.ticks = value;
    code = qom_get_u64(session, "/machine", "pc", &value, error);
    if (code != EMU_OK) return code;
    observed.pc = (uint32_t)value;
    observed.measured_ns = monotonic_ns();
    session->state = observed;
    session->last_stats_ns = observed.measured_ns;
    session->display.icount = observed.icount;
    qemu_ok(error);
    return EMU_OK;
}

static emu_error_code_t verify_surface(emu_qemu_session_t *session,
                                       emu_error_t *error) {
    static const struct {
        const char *path;
        const char *property;
    } required[] = {
        {"/machine", "guest-instructions"},
        {"/machine", "timing-ticks"},
        {"/machine", "pc"},
        {"/machine/keypad", "ui-buttons"},
        {"/machine/keypad", "sampled-buttons"},
        {"/machine/display", "frame-count"},
    };
    uint64_t ignored;
    if (session->snapshot_requested || session->snapshot_path[0]) {
        if (qom_get_u64(session, "/machine", "snapshot-version", &ignored,
                        error) != EMU_OK ||
            ignored != EMU_QEMU_SNAPSHOT_NATIVE_VERSION)
            return qemu_fail(error, EMU_ERR_UNSUPPORTED,
                             "QEMU binary lacks compatible snapshot support; rebuild the fork");
    }
    emu_error_code_t audio_code = qom_get_u64(
        session, "/machine", "audio-available", &ignored, error);
    if (audio_code != EMU_OK) return audio_code;
    if (ignored > 1)
        return qemu_fail(error, EMU_ERR_UNSUPPORTED,
                         "invalid QEMU audio availability");
    session->state.audio_available = ignored;
    for (size_t i = 0; i < sizeof required / sizeof required[0]; i++) {
        emu_error_code_t code = qom_get_u64(
            session, required[i].path, required[i].property,
            &ignored, error);
        if (code != EMU_OK) return code;
    }
    if (session->product->capabilities & EMU_PRODUCT_BATTERY) {
        emu_error_code_t code = qom_get_u64(
            session, "/machine", "battery-level", &ignored, error);
        if (code != EMU_OK) return code;
    }
    if (session->trace_mask & EMU_TRACE_MASK_EEPROM) {
        emu_error_code_t code = qom_get_u64(
            session, "/machine", "nor-trace-version", &ignored, error);
        if (code != EMU_OK) return code;
        if (ignored != 1)
            return qemu_fail(error, EMU_ERR_UNSUPPORTED,
                             "unsupported QEMU physical NOR trace version");
    }
    return update_stats(session, error);
}

static void qemu_frame(void *opaque, const emu_frame_t *frame) {
    emu_qemu_session_t *session = opaque;
    if (session->callbacks.frame)
        session->callbacks.frame(session->callbacks.opaque, frame);
    if (session->callbacks.capture_frame) {
        unsigned width = frame->width * 4u;
        unsigned height = frame->height * 4u;
        size_t size = (size_t)width * height * 3u;
        uint8_t *rgb = malloc(size);

        if (!rgb) {
            session->display.failed = 1;
            snprintf(session->display.failure,
                     sizeof session->display.failure,
                     "cannot allocate QEMU capture frame");
            return;
        }
        for (unsigned y = 0; y < frame->height; y++) {
            for (unsigned x = 0; x < frame->width; x++) {
                const uint8_t *pixel = frame->rgb +
                    ((size_t)y * frame->width + x) * 3u;

                for (unsigned sy = 0; sy < 4; sy++) {
                    for (unsigned sx = 0; sx < 4; sx++) {
                        memcpy(rgb + (((size_t)y * 4u + sy) * width +
                                      x * 4u + sx) * 3u,
                               pixel, 3u);
                    }
                }
            }
        }
        emu_frame_t capture = *frame;
        capture.width = width;
        capture.height = height;
        capture.rgb = rgb;
        capture.rgb_size = size;
        session->callbacks.capture_frame(session->callbacks.opaque,
                                         &capture);
        free(rgb);
    }
}

static emu_error_code_t qemu_create(
        const emu_prepared_session_t *prepared,
        const emu_callbacks_t *callbacks, void **output,
        emu_error_t *error) {
    if (!prepared || !output || *output ||
        !prepared->options.qemu_binary[0])
        return qemu_fail(error, EMU_ERR_ARGUMENT,
                         "missing QEMU binary or session output");
    if (prepared->options.gdb_enabled && !prepared->options.gdb_socket[0] &&
        (prepared->options.gdb_port == 0 ||
         prepared->options.gdb_port > 65535u))
        return qemu_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                         "invalid QEMU GDB port");
    if (access(prepared->options.qemu_binary, X_OK))
        return qemu_fail(error, EMU_ERR_IO,
                         "QEMU binary is not executable: %s",
                         prepared->options.qemu_binary);
    emu_qemu_session_t *session = calloc(1, sizeof *session);
    if (!session)
        return qemu_fail(error, EMU_ERR_NOMEM,
                         "cannot allocate QEMU session");
    session->asc_fd = -1;
    session->stderr_fd = -1;
    session->stdout_fd = -1;
    session->qmp.fd = -1;
    session->serial_attachment = EMU_SERIAL_LINK_DETACHED;
    session->trace_mask = prepared->options.trace_mask;
    session->prepared = prepared;
    session->sim_attached = prepared->options.sim_stub;
    session->snapshot_requested = prepared->options.snapshot_requested;
    session->gdb_port = prepared->options.gdb_port;
    session->gdb_enabled = prepared->options.gdb_enabled;
    snprintf(session->gdb_socket, sizeof session->gdb_socket, "%s", prepared->options.gdb_socket);
    session->product = emu_product_by_name(prepared->selected_device);
    if (callbacks) session->callbacks = *callbacks;
    emu_qemu_audio_init(&session->audio, callbacks);
    emu_qemu_display_init(&session->display, qemu_frame, session,
                          session->product ? session->product->display.width : 0,
                          session->product ? session->product->display.height : 0);
    emu_error_code_t code = emu_qemu_image_materialize(
        prepared, &session->image, error);
    if (code == EMU_OK && prepared->options.snapshot_path) {
        if (strlen(prepared->options.snapshot_path) >= sizeof session->snapshot_path)
            code = qemu_fail(error, EMU_ERR_ARGUMENT, "snapshot path is too long");
        else {
            strcpy(session->snapshot_path, prepared->options.snapshot_path);
            code = emu_qemu_snapshot_read(session->snapshot_path,
                                          &session->snapshot, error);
        }
        if (code == EMU_OK)
            code = emu_qemu_snapshot_validate(session->snapshot_path,
                      &session->snapshot, prepared, error);
        if (code == EMU_OK)
            code = emu_qemu_snapshot_materialize(session->snapshot_path,
                      &session->snapshot, &session->image, error);
    }
    if (code == EMU_OK && (session->snapshot_requested || session->snapshot_path[0])) {
        char *source = realpath(prepared->source.locator, NULL);
        if (!source || strlen(source) >= sizeof session->snapshot.flash)
            code = qemu_fail(error, EMU_ERR_ARGUMENT, "invalid snapshot source path");
        else strcpy(session->snapshot.flash, source);
        free(source);
        strcpy(session->snapshot.flash_sha256, prepared->source.sha256_hex);
        session->snapshot.flash_size = prepared->source.size;
        strcpy(session->snapshot.device, prepared->selected_device);
        session->snapshot.firmware_patches = prepared->options.firmware_patches;
        session->snapshot.sim_stub = prepared->options.sim_stub;
        session->snapshot.native_version = EMU_QEMU_SNAPSHOT_NATIVE_VERSION;
    }
    if (code == EMU_OK &&
        (session->trace_mask & ~EMU_TRACE_MASK_FIRMWARE_PATCH)) {
        int length = snprintf(session->native_trace_path,
                              sizeof session->native_trace_path,
                              "%s/native-trace.log", session->image.directory);
        if (length < 0 || (size_t)length >= sizeof session->native_trace_path)
            code = qemu_fail(error, EMU_ERR_ARGUMENT,
                             "QEMU native trace path is too long");
    }
    if (code == EMU_OK && (session->trace_mask & EMU_TRACE_MASK_EEPROM) &&
        emu_qemu_eeprom_trace_create(&session->eeprom_trace, &session->image,
            session->trace_error, sizeof session->trace_error))
        code = qemu_fail(error, EMU_ERR_IO, "%s", session->trace_error);
    if (code == EMU_OK)
        code = spawn_qemu(session, prepared->options.qemu_binary, error);
    if (code == EMU_OK)
        code = emu_qmp_handshake(&session->qmp, 5000, error);
    if (code == EMU_OK)
        snprintf(qemu_version, sizeof qemu_version, "%s",
                 session->qmp.version);
    if (code == EMU_OK)
        code = verify_surface(session, error);
    if (code == EMU_OK)
        code = emu_qemu_display_attach(&session->display,
                                       &session->qmp, error);
    if (code == EMU_OK && session->state.audio_available)
        code = emu_qemu_audio_attach(&session->audio,
                                      session->display.control, error);
    if (code != EMU_OK) {
        if (session->alive) kill(session->child, SIGKILL);
        if (session->alive) waitpid(session->child, NULL, 0);
        if (session->stderr_fd >= 0) read_stderr(session);
        read_stdout(session);
        if (error && session->stderr_size) {
            size_t used = strlen(error->message);
            snprintf(error->message + used, sizeof error->message - used,
                     "%s%s", used ? ": " : "", session->stderr_text);
        }
        if (session->qmp.fd >= 0) close(session->qmp.fd);
        if (session->asc_fd >= 0) close(session->asc_fd);
        if (session->stderr_fd >= 0) close(session->stderr_fd);
        if (session->stdout_fd >= 0) close(session->stdout_fd);
        emu_qemu_audio_destroy(&session->audio);
        emu_qemu_display_destroy(&session->display);
        if (session->native_trace_path[0])
            unlink(session->native_trace_path);
        emu_qemu_eeprom_trace_destroy(&session->eeprom_trace);
        emu_qemu_image_destroy(&session->image);
        if (session->host_trace_tail) fclose(session->host_trace_tail);
        free(session);
        return code;
    }
    *output = session;
    qemu_ok(error);
    return EMU_OK;
}

static emu_error_code_t qemu_start(void *opaque, emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    if (!session || !session->alive || session->started ||
        (session->snapshot_path[0] && !session->restored))
        return qemu_fail(error, EMU_ERR_LIFECYCLE,
                         "QEMU session cannot be started");
    if (session->gdb_enabled) {
        session->started = 1;
        qemu_ok(error);
        return EMU_OK;
    }
    emu_error_code_t code = emu_qmp_command(
        &session->qmp, "cont", NULL, -1, NULL, 0, 5000, error);
    if (code == EMU_OK) session->started = 1;
    return code;
}

static emu_error_code_t qemu_sim(void *opaque, int attached,
                                 emu_error_t *error) {
    emu_qemu_session_t *session = opaque;

    if (!session || !session->alive)
        return qemu_fail(error, EMU_ERR_LIFECYCLE,
                         "QEMU SIM session is unavailable");
    if (!!attached != !!session->sim_attached)
        return qemu_fail(error, EMU_ERR_UNSUPPORTED,
                         "QEMU SIM attachment is fixed before reset");
    qemu_ok(error);
    return EMU_OK;
}

static int parse_native_line(emu_qemu_session_t *session, const char *line) {
    int status = emu_qemu_eeprom_trace_parse(session->eeprom_trace, line,
        session->trace_error, sizeof session->trace_error);
    if (!status)
        status = emu_qemu_trace_parse_line(session->trace_sink, line,
            session->trace_error, sizeof session->trace_error);
    if (status < 0) session->trace_sink->failed = 1;
    return status;
}

static void trace_byte(emu_qemu_session_t *session, char byte) {
    if (!session->trace_sink || session->trace_failed) return;
    if (byte != '\n') {
        if (session->trace_line_size + 1u >= sizeof session->trace_line) {
            session->trace_failed = 1;
            snprintf(session->trace_error, sizeof session->trace_error,
                     "oversized QEMU trace log record");
            return;
        }
        session->trace_line[session->trace_line_size++] = byte;
        return;
    }
    session->trace_line[session->trace_line_size] = 0;
    if (parse_native_line(session, session->trace_line) < 0)
        session->trace_failed = 1;
    session->trace_line_size = 0;
}

static void read_stderr(emu_qemu_session_t *session) {
    char bytes[4096];
    for (;;) {
        ssize_t count = read(session->stderr_fd, bytes, sizeof bytes);
        if (count > 0) {
            size_t available = sizeof session->stderr_text -
                               session->stderr_size - 1u;
            size_t keep = (size_t)count < available ? (size_t)count : available;
            if (keep) {
                memcpy(session->stderr_text + session->stderr_size,
                       bytes, keep);
                session->stderr_size += keep;
            }
            session->stderr_text[session->stderr_size] = 0;
            if (!session->native_trace_path[0])
                for (ssize_t i = 0; i < count; i++)
                    trace_byte(session, bytes[i]);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        break;
    }
}

static int emit_patch_hunk(emu_trace_sink_t *sink,
                           const emu_patch_definition_t *patch,
                           size_t chip_index, size_t chip_offset,
                           const uint8_t *expected,
                           const uint8_t *replacement, size_t size,
                           const char *status) {
    char *old_hex = malloc(size * 2u + 1u);
    char *new_hex = malloc(size * 2u + 1u);
    if (!old_hex || !new_hex ||
        hex_bytes(old_hex, size * 2u + 1u, expected, size) ||
        hex_bytes(new_hex, size * 2u + 1u, replacement, size)) {
        free(old_hex);
        free(new_hex);
        return -1;
    }
    emu_trace_event_t event = {
        .kind = "firmware_patch", .icount = 0, .pc = 0,
        .has_addr = 1, .addr = (uint32_t)chip_offset,
        .has_size = 1, .size = (int)size,
        .detail = patch->description,
    };
    emu_trace_info_int(&event.info, "chip_index", (long)chip_index);
    emu_trace_info_int(&event.info, "chip_offset", (long)chip_offset);
    emu_trace_info_str(&event.info, "expected", old_hex);
    emu_trace_info_str(&event.info, "patch", patch->name);
    emu_trace_info_str(&event.info, "provenance", patch->provenance);
    emu_trace_info_str(&event.info, "replacement", new_hex);
    emu_trace_info_str(&event.info, "status", status);
    emu_trace_emit(sink, &event);
    free(old_hex);
    free(new_hex);
    return sink->failed ? -1 : 0;
}

static int emit_patch_trace(emu_qemu_session_t *session,
                            emu_trace_sink_t *sink) {
    size_t count = 0;
    const emu_patch_definition_t *catalog = emu_patch_catalog(&count);
    for (size_t i = 0; i < count; i++) {
        if (!(session->prepared->options.firmware_patches &
              (UINT64_C(1) << i))) continue;
        const emu_patch_definition_t *patch = &catalog[i];
        for (size_t j = 0; j < patch->hunk_count; j++) {
            const emu_patch_hunk_t *hunk = &patch->hunks[j];
            const char *status = "already_applied";
            if (!session->snapshot_path[0]) {
                char prefix[96];
                snprintf(prefix, sizeof prefix, "firmware-patch:%s:",
                         patch->name);
                int found = 0;
                for (size_t k = 0; k < session->prepared->operation_count; k++) {
                    const emu_storage_operation_t *operation =
                        &session->prepared->operations[k];
                    if (operation->stage != EMU_STORAGE_STAGE_POST_RESTORE ||
                        operation->space != EMU_STORAGE_MAIN_ARRAY ||
                        operation->chip_index != (size_t)hunk->chip_index ||
                        operation->offset != hunk->chip_offset ||
                        strncmp(operation->provenance, prefix,
                                strlen(prefix))) continue;
                    status = session->image.already_applied_operations &
                             (UINT64_C(1) << k) ? "already_applied" : "applied";
                    found = 1;
                    break;
                }
                if (!found) return -1;
            }
            if (emit_patch_hunk(sink, patch, hunk->chip_index,
                                hunk->chip_offset, hunk->expected,
                                hunk->replacement, hunk->size, status))
                return -1;
        }
    }
    return 0;
}

static emu_error_code_t qemu_trace_attach(
        void *opaque, emu_trace_sink_t *sink, int deferred, int gsm_l1,
        emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    if (!session || !sink || session->trace_sink || deferred || gsm_l1 ||
        !session->trace_mask || sink->mask != session->trace_mask)
        return qemu_fail(error, EMU_ERR_ARGUMENT,
                         "invalid QEMU trace attachment");
    if (session->eeprom_trace && emu_qemu_eeprom_trace_attach(
            session->eeprom_trace, sink, session->trace_error,
            sizeof session->trace_error))
        return qemu_fail(error, EMU_ERR_ENGINE, "%s", session->trace_error);
    if ((session->trace_mask & EMU_TRACE_MASK_FIRMWARE_PATCH) &&
        emit_patch_trace(session, sink))
        return qemu_fail(error, EMU_ERR_ENGINE,
                         "cannot emit QEMU firmware patch trace");
    session->trace_sink = sink;
    qemu_ok(error);
    return EMU_OK;
}

static emu_error_code_t qemu_trace_host_event(
        void *opaque, const emu_trace_event_t *event, emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    char record[EMU_QEMU_HOST_EVENT_MAX];
    if (emu_qemu_trace_host_encode(event, record, sizeof record))
        return qemu_fail(error, EMU_ERR_ARGUMENT, "invalid QEMU host service event");
    if (!session->trace_sink || !emu_trace_sink_accepts(session->trace_sink, event->kind)) {
        qemu_ok(error);
        return EMU_OK;
    }
    if (!session->alive || session->stop_requested) {
        /* Post-stop callbacks follow all native records. Spool their bounded
         * records to disk, then replay after the native producer has closed. */
        if (!session->host_trace_tail) session->host_trace_tail = tmpfile();
        if (!session->host_trace_tail || fprintf(session->host_trace_tail,
                "siemens_host_service icount=%llu event=%s\n",
                (unsigned long long)session->state.icount, record) < 0)
            return qemu_fail(error, EMU_ERR_IO, "cannot spool host trace tail");
        qemu_ok(error);
        return EMU_OK;
    }
    char arguments[EMU_QEMU_HOST_EVENT_MAX + 128];
    snprintf(arguments, sizeof arguments,
        "{\"path\":\"/machine\",\"property\":\"host-service-event\",\"value\":\"%s\"}", record);
    return emu_qmp_command(&session->qmp, "qom-set", arguments, -1,
                            NULL, 0, 5000, error);
}

static emu_error_code_t qemu_trace_detach(void *opaque, emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    if (!session || !session->trace_sink)
        return qemu_fail(error, EMU_ERR_LIFECYCLE,
                         "QEMU trace is not attached");
    if (session->stderr_fd >= 0) read_stderr(session);
    if (session->native_trace_path[0] && !session->trace_failed) {
        FILE *trace = fopen(session->native_trace_path, "r");
        char *line = NULL;
        size_t capacity = 0;
        if (!trace) {
            session->trace_failed = 1;
            snprintf(session->trace_error, sizeof session->trace_error,
                     "cannot open QEMU trace log: %s", strerror(errno));
        } else {
            while (!session->trace_failed && getline(&line, &capacity, trace) >= 0) {
                if (parse_native_line(session, line) < 0)
                    session->trace_failed = 1;
            }
            if (ferror(trace) && !session->trace_failed) {
                session->trace_failed = 1;
                snprintf(session->trace_error, sizeof session->trace_error,
                         "cannot read QEMU trace log: %s", strerror(errno));
            }
            free(line);
            fclose(trace);
        }
    } else if (session->trace_line_size && !session->trace_failed) {
        trace_byte(session, '\n');
    }
    if (session->host_trace_tail) {
        if (fflush(session->host_trace_tail) || fseek(session->host_trace_tail, 0, SEEK_SET))
            session->trace_failed = 1;
        char line[EMU_QEMU_HOST_EVENT_MAX + 128];
        while (!session->trace_failed && fgets(line, sizeof line, session->host_trace_tail))
            if (parse_native_line(session, line) < 0) session->trace_failed = 1;
        if (ferror(session->host_trace_tail)) session->trace_failed = 1;
        if (fclose(session->host_trace_tail)) session->trace_failed = 1;
        session->host_trace_tail = NULL;
    }
    if (session->trace_failed) session->trace_sink->failed = 1;
    emu_qemu_eeprom_trace_detach(session->eeprom_trace);
    session->trace_sink = NULL;
    if (!session->trace_failed) {
        qemu_ok(error);
        return EMU_OK;
    }
    return qemu_fail(error, EMU_ERR_ENGINE, "%s",
                     session->trace_error[0] ? session->trace_error :
                     "QEMU trace finalization failed");
}

static emu_error_code_t pump_io(emu_qemu_session_t *session,
                                unsigned timeout_ms,
                                emu_error_t *error) {
    struct pollfd descriptors[] = {
        {.fd = session->asc_fd, .events = POLLIN},
        {.fd = session->stderr_fd, .events = POLLIN},
        {.fd = session->stdout_fd, .events = POLLIN},
    };
    /* D-Bus audio/display readiness must wake the same poll as native I/O.
     * Registration and dispatch stay on the session's main thread. */
    GMainContext *context = g_main_context_default();
    if (!g_main_context_acquire(context))
        return qemu_fail(error, EMU_ERR_IO, "QEMU main context is already owned");
    gint priority, context_timeout, needed, count = 0;
    gboolean ready = g_main_context_prepare(context, &priority);
    GPollFD *fds = NULL;
    do {
        needed = g_main_context_query(context, priority, &context_timeout,
                                      fds ? fds + 3 : NULL, count);
        if (needed <= count) break;
        count = needed;
        fds = g_renew(GPollFD, fds, count + 3);
    } while (TRUE);
    if (!fds) fds = g_new(GPollFD, 3);
    for (unsigned i = 0; i < 3; i++)
        fds[i] = (GPollFD){.fd = descriptors[i].fd, .events = G_IO_IN};
    gint wait_ms = (gint)MIN(timeout_ms, (unsigned)G_MAXINT);
    if (ready) wait_ms = 0;
    else if (context_timeout >= 0) wait_ms = MIN(wait_ms, context_timeout);
    int status = g_poll(fds, needed + 3, wait_ms);
    int saved_errno = errno;
    for (unsigned i = 0; i < 3; i++) descriptors[i].revents = fds[i].revents;
    if (status >= 0 && g_main_context_check(context, priority, fds + 3, needed))
        g_main_context_dispatch(context);
    g_free(fds);
    g_main_context_release(context);
    if (status < 0 && saved_errno != EINTR)
        return qemu_fail(error, EMU_ERR_IO,
                         "QEMU transport poll failed: %s", strerror(saved_errno));
    if (descriptors[0].revents & POLLIN) {
        uint8_t bytes[4096];
        for (;;) {
            ssize_t count = read(session->asc_fd, bytes, sizeof bytes);
            if (count > 0) {
                session->serial_tx_bytes += (uint64_t)count;
                if (session->callbacks.serial)
                    session->callbacks.serial(session->callbacks.opaque,
                                              bytes, (size_t)count);
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            break;
        }
    }
    if (descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
        close(session->asc_fd);
        session->asc_fd = -1;
    }
    if (descriptors[1].revents & (POLLIN | POLLHUP)) read_stderr(session);
    if (descriptors[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
        close(session->stderr_fd);
        session->stderr_fd = -1;
    }
    if (descriptors[2].revents) read_stdout(session);
    emu_error_code_t code = emu_qemu_display_pump(&session->display, error);
    if (code == EMU_OK && !session->stop_requested)
        code = emu_qemu_audio_pump(&session->audio, error);
    return code;
}

static void fill_result(emu_qemu_session_t *session,
                        emu_run_result_t *result) {
    *result = (emu_run_result_t) {
        .status = EMU_RUN_STOPPED,
        .ticks = session->state.ticks - session->initial_ticks,
        .guest_instructions = session->state.guest_instructions -
                              session->initial_instructions,
        .end_icount = session->state.icount,
        .pc = session->state.pc,
    };
}

static emu_error_code_t qemu_pump(void *opaque, unsigned timeout_ms,
                                  int *complete, emu_run_result_t *result,
                                  emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    if (!session || !complete || !result || !session->alive)
        return qemu_fail(error, EMU_ERR_LIFECYCLE,
                         "invalid QEMU pump request");
    emu_error_code_t code = pump_io(session, timeout_ms, error);
    if (code != EMU_OK) return code;
    int status = 0;
    pid_t waited = waitpid(session->child, &status, WNOHANG);
    if (waited < 0)
        return qemu_fail(error, EMU_ERR_IO,
                         "cannot reap QEMU: %s", strerror(errno));
    if (waited == session->child) {
        session->alive = 0;
        session->exit_status = status;
        read_stderr(session);
        read_stdout(session);
        fill_result(session, result);
        *complete = 1;
        if (!session->stop_requested &&
            (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
            return qemu_fail(error, EMU_ERR_ENGINE,
                "QEMU exited unexpectedly%s%s",
                session->stderr_size ? ": " : "",
                session->stderr_size ? session->stderr_text : "");
        qemu_ok(error);
        return EMU_OK;
    }
    uint64_t now = monotonic_ns();
    if (!session->stop_requested && (!session->last_stats_ns ||
        now - session->last_stats_ns >= UINT64_C(10000000))) {
        code = update_stats(session, error);
        if (code != EMU_OK) return code;
    }
    fill_result(session, result);
    *complete = 0;
    qemu_ok(error);
    return EMU_OK;
}

static emu_error_code_t qemu_reset(void *opaque, emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    if (!session || !session->alive)
        return qemu_fail(error, EMU_ERR_LIFECYCLE,
                         "QEMU session is not live");
    session->ui_buttons = 0;
    session->initial_ticks = session->initial_instructions = 0;
    return emu_qmp_command(&session->qmp, "system_reset", NULL, -1,
                           NULL, 0, 5000, error);
}

static emu_error_code_t qemu_stop(void *opaque, emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    if (!session || !session->alive)
        return qemu_fail(error, EMU_ERR_LIFECYCLE,
                         "QEMU session is not live");
    session->stop_requested = 1;
    emu_error_code_t code = emu_qmp_command(
        &session->qmp, "stop", NULL, -1, NULL, 0, 5000, error);
    if (code != EMU_OK) return code;
    code = update_stats(session, error);
    if (code != EMU_OK) return code;
    /* QEMU's quit path performs bdrv_flush_all() before block teardown. */
    return emu_qmp_command(&session->qmp, "quit", NULL, -1,
                           NULL, 0, 5000, error);
}

static emu_error_code_t qemu_query(void *opaque, int detailed,
                                   emu_engine_state_t *state,
                                   emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    if (!session || !state)
        return qemu_fail(error, EMU_ERR_ARGUMENT, "invalid QEMU query");
    if (detailed && session->alive) {
        emu_error_code_t code = update_stats(session, error);
        if (code != EMU_OK) return code;
    }
    *state = session->state;
    state->ticks -= session->initial_ticks;
    state->guest_instructions -= session->initial_instructions;
    state->stop_requested = session->stop_requested;
    qemu_ok(error);
    return EMU_OK;
}

static emu_error_code_t qemu_key(void *opaque, const char *name, int pressed,
                                 emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    const emu_logical_key_t *key =
        session && session->product ?
        emu_product_key(session->product, name) : NULL;
    if (!key)
        return qemu_fail(error, EMU_ERR_ARGUMENT, "unknown QEMU key");
    size_t index = (size_t)(key - session->product->keys);
    uint32_t mask = 1u << index;
    uint64_t current;
    emu_error_code_t code = qom_get_u64(
        session, "/machine/keypad", "ui-buttons", &current, error);
    if (code != EMU_OK) return code;
    uint32_t next = pressed ? (uint32_t)current | mask :
                              (uint32_t)current & ~mask;
    code = qom_set_u64(
        session, "/machine/keypad", "ui-buttons", next, error);
    if (code == EMU_OK) session->ui_buttons = next;
    return code;
}

static emu_error_code_t qemu_key_query(void *opaque, const char *name,
                                       emu_key_sample_t *sample,
                                       emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    const emu_logical_key_t *key =
        session && session->product ?
        emu_product_key(session->product, name) : NULL;
    if (!key || !sample)
        return qemu_fail(error, EMU_ERR_ARGUMENT, "unknown QEMU key");
    uint64_t sampled;
    uint64_t requested;
    emu_error_code_t code = qom_get_u64(
        session, "/machine/keypad", "ui-buttons", &requested, error);
    if (code != EMU_OK) return code;
    code = qom_get_u64(
        session, "/machine/keypad", "sampled-buttons", &sampled, error);
    if (code != EMU_OK) return code;
    session->ui_buttons = (uint32_t)requested;
    session->sampled_buttons = (uint32_t)sampled;
    size_t index = (size_t)(key - session->product->keys);
    uint32_t mask = 1u << index;
    *sample = (emu_key_sample_t) {
        .requested_pressed = !!(session->ui_buttons & mask),
        .sampled_pressed = !!(session->sampled_buttons & mask),
        .icount = session->state.icount,
    };
    qemu_ok(error);
    return EMU_OK;
}

static emu_error_code_t qemu_serial(void *opaque, const uint8_t *bytes,
                                    size_t size, emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    if (!session || (!bytes && size))
        return qemu_fail(error, EMU_ERR_ARGUMENT,
                         "invalid QEMU serial input");
    size_t written = 0;
    while (written < size) {
        ssize_t count = write(session->asc_fd, bytes + written,
                              size - written);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return qemu_fail(error, EMU_ERR_ENGINE,
                             "QEMU ASC0 input is backpressured");
        if (count <= 0)
            return qemu_fail(error, EMU_ERR_IO,
                             "cannot write QEMU ASC0: %s", strerror(errno));
        written += (size_t)count;
    }
    session->serial_rx_bytes += size;
    qemu_ok(error);
    return EMU_OK;
}

static emu_error_code_t qemu_serial_link(
        void *opaque, emu_serial_link_attachment_t attachment,
        emu_serial_link_state_t *state, emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    if (!session || !state)
        return qemu_fail(error, EMU_ERR_ARGUMENT,
                         "invalid QEMU serial link request");
    session->serial_attachment = attachment;
    *state = (emu_serial_link_state_t) {
        .attachment = attachment,
        .available = session->alive,
        .rx_bytes = session->serial_rx_bytes,
        .tx_bytes = session->serial_tx_bytes,
    };
    qemu_ok(error);
    return EMU_OK;
}

static emu_error_code_t qemu_battery(void *opaque, unsigned level,
                                     int charging, emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    if (!session || level > 100u)
        return qemu_fail(error, EMU_ERR_ARGUMENT,
                         "invalid QEMU battery state");
    emu_error_code_t code = qom_set_u64(
        session, "/machine", "battery-level", level, error);
    return code == EMU_OK ? qom_set_bool(
        session, "/machine", "battery-charging", charging, error) : code;
}

static emu_error_code_t qemu_poll(void *opaque, emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    if (!session)
        return qemu_fail(error, EMU_ERR_ARGUMENT, "invalid QEMU poll");
    return pump_io(session, 0, error);
}

static emu_error_code_t qemu_flash_size(void *opaque, size_t *size,
                                        emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    if (!session || !size)
        return qemu_fail(error, EMU_ERR_ARGUMENT,
                         "invalid QEMU flash size request");
    *size = session->image.combined_size;
    qemu_ok(error);
    return EMU_OK;
}

static emu_error_code_t qemu_flash_read(void *opaque, size_t offset,
                                        uint8_t *bytes, size_t size,
                                        emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    if (!session || (!bytes && size) ||
        offset > session->image.combined_size ||
        size > session->image.combined_size - offset)
        return qemu_fail(error, EMU_ERR_ARGUMENT,
                         "invalid QEMU flash read request");
    return emu_qemu_image_read(&session->image, offset, bytes, size, error);
}

/* Both directions use a passed file descriptor: paths never become shell or
 * QMP syntax, and outgoing files are reserved without replacing old snapshots. */
static emu_error_code_t migrate_file(emu_qemu_session_t *session, int fd,
                                     int incoming, emu_error_t *error) {
    emu_error_code_t code = emu_qmp_command(&session->qmp, "add-fd",
        "{\"fdset-id\":7850}", fd, NULL, 0, 5000, error);
    if (code != EMU_OK) return code;
    code = emu_qmp_command(&session->qmp,
        incoming ? "migrate-incoming" : "migrate",
        "{\"channels\":[{\"channel-type\":\"main\",\"addr\":{"
        "\"transport\":\"file\",\"filename\":\"/dev/fdset/7850\",\"offset\":0}}]}",
        -1, NULL, 0, 5000, error);
    if (code != EMU_OK) goto done;
    uint64_t deadline = monotonic_ns() + UINT64_C(120000000000);
    do {
        char response[8192];
        code = emu_qmp_command(&session->qmp, "query-migrate", NULL, -1,
                               response, sizeof response, 5000, error);
        if (code != EMU_OK) goto done;
        const char *status = strstr(response, "\"status\"");
        if (status && (status = strchr(status, ':'))) {
            status++;
            while (*status == ' ' || *status == '\t') status++;
            if (!strncmp(status, "\"completed\"", 11)) goto done;
            if (!strncmp(status, "\"failed\"", 8) ||
                !strncmp(status, "\"cancelled\"", 11)) {
                code = qemu_fail(error, EMU_ERR_ENGINE,
                                 "QEMU snapshot migration failed: %.350s", response);
                goto done;
            }
        }
        code = pump_io(session, 10, error);
        if (code != EMU_OK) goto done;
    } while (monotonic_ns() < deadline);
    code = qemu_fail(error, EMU_ERR_ENGINE, "QEMU snapshot migration timed out");
done:;
    emu_error_t ignored;
    emu_qmp_command(&session->qmp, "remove-fd", "{\"fdset-id\":7850}",
                     -1, NULL, 0, 5000, &ignored);
    return code;
}

static emu_error_code_t qemu_snapshot_restore(void *opaque, const char *dir,
                                              emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    if (!session || !session->alive || session->started || session->restored ||
        !dir || strcmp(dir, session->snapshot_path))
        return qemu_fail(error, EMU_ERR_LIFECYCLE, "invalid QEMU snapshot restore");
    char path[EMU_QEMU_PATH_MAX];
    if (emu_qemu_snapshot_path(path, sizeof path, dir, "vmstate.bin"))
        return qemu_fail(error, EMU_ERR_ARGUMENT, "snapshot path is too long");
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return qemu_fail(error, EMU_ERR_IO, "cannot open snapshot VMState");
    emu_error_code_t code = migrate_file(session, fd, 1, error);
    close(fd);
    if (code == EMU_OK) code = update_stats(session, error);
    if (code == EMU_OK && (session->state.icount != session->snapshot.icount ||
        session->state.ticks != session->snapshot.ticks ||
        session->state.pc != session->snapshot.pc))
        code = qemu_fail(error, EMU_ERR_ENGINE, "restored QEMU state disagrees with snapshot manifest");
    if (code == EMU_OK) {
        session->initial_ticks = session->state.ticks;
        session->initial_instructions = session->state.guest_instructions;
        session->restored = 1;
    }
    return code;
}

static emu_error_code_t qemu_snapshot_write(void *opaque, const char *dir,
    const char *host_json, int full_bins, emu_error_t *error) {
    emu_qemu_session_t *session = opaque;
    (void)host_json;
    if (!session || !session->alive || session->stop_requested ||
        !session->snapshot_requested || !dir || !*dir || full_bins)
        return qemu_fail(error, EMU_ERR_LIFECYCLE, "invalid QEMU snapshot write");
    emu_error_code_t code = emu_qmp_command(&session->qmp, "stop", NULL, -1,
                                            NULL, 0, 5000, error);
    if (code == EMU_OK) code = update_stats(session, error);
    if (code != EMU_OK) return code;
    if (mkdir(dir, 0700) && errno != EEXIST)
        return qemu_fail(error, EMU_ERR_IO, "cannot create snapshot directory: %s", strerror(errno));
    char path[EMU_QEMU_PATH_MAX];
    if (emu_qemu_snapshot_path(path, sizeof path, dir, "snapshot.json") ||
        access(path, F_OK) == 0)
        return qemu_fail(error, EMU_ERR_IO, "snapshot destination already exists or is invalid");
    if (emu_qemu_snapshot_path(path, sizeof path, dir, "vmstate.bin"))
        return qemu_fail(error, EMU_ERR_ARGUMENT, "snapshot path is too long");
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return qemu_fail(error, EMU_ERR_IO, "cannot create snapshot VMState: %s", strerror(errno));
    code = migrate_file(session, fd, 0, error);
    if (code == EMU_OK && fsync(fd))
        code = qemu_fail(error, EMU_ERR_IO, "cannot sync snapshot VMState");
    if (close(fd) && code == EMU_OK)
        code = qemu_fail(error, EMU_ERR_IO, "cannot close snapshot VMState");
    if (code == EMU_OK) {
        session->snapshot.icount = session->state.icount;
        session->snapshot.ticks = session->state.ticks;
        session->snapshot.pc = session->state.pc;
        snprintf(session->snapshot.qemu_version, sizeof session->snapshot.qemu_version,
                 "%.40s (%.80s)", session->qmp.version, session->qmp.build);
        code = emu_qemu_snapshot_commit(dir, &session->snapshot, &session->image, error);
    }
    if (code != EMU_OK) {
        emu_error_t ignored;
        emu_qmp_command(&session->qmp, "migrate_cancel", NULL, -1,
                         NULL, 0, 5000, &ignored);
        unlink(path);
    }
    return code;
}

static void qemu_destroy(void *opaque) {
    emu_qemu_session_t *session = opaque;
    if (!session) return;
    if (session->alive) {
        emu_error_t ignored = {0};
        if (!session->stop_requested) qemu_stop(session, &ignored);
        for (unsigned i = 0; i < 50u && session->alive; i++) {
            int status;
            pid_t waited = waitpid(session->child, &status, WNOHANG);
            if (waited == session->child) {
                session->alive = 0;
                session->exit_status = status;
                break;
            }
            read_stdout(session);
            poll(NULL, 0, 10);
        }
    }
    if (session->alive) {
        kill(session->child, SIGTERM);
        for (unsigned i = 0; i < 50u; i++) {
            int status;
            pid_t waited = waitpid(session->child, &status, WNOHANG);
            if (waited == session->child) {
                session->alive = 0;
                break;
            }
            read_stdout(session);
            poll(NULL, 0, 10);
        }
    }
    if (session->alive) {
        kill(session->child, SIGKILL);
        waitpid(session->child, NULL, 0);
    }
    read_stdout(session);
    if (session->stdout_fd >= 0) close(session->stdout_fd);
    emu_qemu_audio_destroy(&session->audio);
    emu_qemu_display_destroy(&session->display);
    if (session->qmp.fd >= 0) close(session->qmp.fd);
    if (session->asc_fd >= 0) close(session->asc_fd);
    if (session->stderr_fd >= 0) close(session->stderr_fd);
    if (session->native_trace_path[0]) unlink(session->native_trace_path);
    emu_qemu_eeprom_trace_destroy(&session->eeprom_trace);
    emu_qemu_image_destroy(&session->image);
    if (session->host_trace_tail) fclose(session->host_trace_tail);
    free(session);
}

static const emu_engine_diagnostic_ops_t qemu_diagnostic_ops = {
    .snapshot_restore = qemu_snapshot_restore,
    .snapshot_write = qemu_snapshot_write,
    .trace_attach = qemu_trace_attach,
    .trace_detach = qemu_trace_detach,
    .trace_host_event = qemu_trace_host_event,
    .flash_size = qemu_flash_size,
    .flash_read = qemu_flash_read,
};

static const emu_engine_diagnostics_t qemu_diagnostics = {
    .engine_name = "qemu",
    .profile_name = "supervised",
    .capabilities = EMU_DIAG_ARTIFACTS | EMU_DIAG_MANAGED_GDB |
                    EMU_DIAG_SNAPSHOT | EMU_DIAG_SNAPSHOT_RESTORE,
    .trace_available = 1,
    .trace_mask = EMU_QEMU_TRACE_MASK,
};

const emu_engine_descriptor_t emu_qemu_engine_descriptor = {
    .name = "qemu",
    .version = qemu_version,
    .supported_models = qemu_models,
    .supported_model_count = sizeof qemu_models / sizeof qemu_models[0],
    .execution_model = EMU_EXECUTION_CONTINUOUS,
    .capabilities = EMU_CAP_KEYS | EMU_CAP_SERIAL | EMU_CAP_BATTERY |
                    EMU_CAP_SIM | EMU_CAP_AUDIO |
                    EMU_CAP_STORAGE_INIT | EMU_CAP_FRAMES |
                    EMU_CAP_KEY_SAMPLING | EMU_CAP_SERIAL_LINK |
                    EMU_CAP_EVENTS,
    .diagnostics = &qemu_diagnostics,
    .diagnostic_ops = &qemu_diagnostic_ops,
    .ops = {
        .create = qemu_create,
        .start = qemu_start,
        .pump = qemu_pump,
        .reset = qemu_reset,
        .request_stop = qemu_stop,
        .query = qemu_query,
        .destroy = qemu_destroy,
        .key = qemu_key,
        .serial_rx = qemu_serial,
        .battery = qemu_battery,
        .sim = qemu_sim,
        .poll = qemu_poll,
        .key_query = qemu_key_query,
        .serial_link = qemu_serial_link,
    },
};
