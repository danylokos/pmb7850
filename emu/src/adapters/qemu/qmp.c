#define _POSIX_C_SOURCE 200809L

#include "emu_qmp.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

static emu_error_code_t qmp_fail(emu_error_t *error,
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

static void qmp_ok(emu_error_t *error) {
    if (error) {
        error->code = EMU_OK;
        error->message[0] = 0;
    }
}

void emu_qmp_init(emu_qmp_t *qmp, int fd) {
    if (!qmp) return;
    memset(qmp, 0, sizeof *qmp);
    qmp->fd = fd;
    qmp->next_id = 1;
}

static int valid_command(const char *command) {
    if (!command || !command[0]) return 0;
    for (const unsigned char *p = (const unsigned char *)command; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return 0;
    return 1;
}

static int send_bytes(int fd, const char *bytes, size_t size, int passed_fd) {
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                   &enabled, sizeof enabled)) return -1;
#endif
#ifdef MSG_NOSIGNAL
    const int send_flags = MSG_NOSIGNAL;
#else
    const int send_flags = 0;
#endif
    size_t sent = 0;
    if (passed_fd >= 0) {
        char control[CMSG_SPACE(sizeof passed_fd)] = {0};
        struct iovec iov = {.iov_base = (void *)bytes, .iov_len = size};
        struct msghdr message = {
            .msg_iov = &iov,
            .msg_iovlen = 1,
            .msg_control = control,
            .msg_controllen = sizeof control,
        };
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof passed_fd);
        memcpy(CMSG_DATA(header), &passed_fd, sizeof passed_fd);
        ssize_t count;
        do {
            count = sendmsg(fd, &message, send_flags);
        } while (count < 0 && errno == EINTR);
        if (count <= 0) return -1;
        sent = (size_t)count;
    }
    while (sent < size) {
        struct iovec iov = {
            .iov_base = (void *)(bytes + sent), .iov_len = size - sent,
        };
        struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1};
        ssize_t count = sendmsg(fd, &message, send_flags);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        sent += (size_t)count;
    }
    return 0;
}

static int take_line(emu_qmp_t *qmp, char *message, size_t capacity) {
    char *newline = memchr(qmp->input, '\n', qmp->input_size);
    if (!newline) return 0;
    size_t length = (size_t)(newline - qmp->input);
    if (length && qmp->input[length - 1u] == '\r') length--;
    if (length + 1u > capacity) return -1;
    memcpy(message, qmp->input, length);
    message[length] = 0;
    size_t consumed = (size_t)(newline - qmp->input) + 1u;
    memmove(qmp->input, qmp->input + consumed,
            qmp->input_size - consumed);
    qmp->input_size -= consumed;
    return 1;
}

static uint64_t monotonic_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value)) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000) +
           (uint64_t)value.tv_nsec / UINT64_C(1000000);
}

static int valid_json_shape(const char *message) {
    char stack[EMU_QMP_NESTING_MAX];
    size_t depth = 0;
    int quoted = 0;
    int escaped = 0;
    for (const unsigned char *cursor = (const unsigned char *)message;
         *cursor; cursor++) {
        if (quoted) {
            if (escaped) escaped = 0;
            else if (*cursor == '\\') escaped = 1;
            else if (*cursor == '"') quoted = 0;
            else if (*cursor < 0x20u) return 0;
            continue;
        }
        if (*cursor == '"') {
            quoted = 1;
        } else if (*cursor == '{' || *cursor == '[') {
            if (depth == sizeof stack) return 0;
            stack[depth++] = (char)*cursor;
        } else if (*cursor == '}' || *cursor == ']') {
            if (!depth || (*cursor == '}' && stack[depth - 1u] != '{') ||
                (*cursor == ']' && stack[depth - 1u] != '[')) return 0;
            depth--;
        }
    }
    return !quoted && !depth && message[0] == '{';
}

int emu_qmp_next_message(emu_qmp_t *qmp, char *message, size_t capacity,
                         unsigned timeout_ms, emu_error_t *error) {
    if (!qmp || qmp->fd < 0 || !message || capacity < 2u) {
        qmp_fail(error, EMU_ERR_ARGUMENT, "invalid QMP read request");
        return -1;
    }
    uint64_t deadline = monotonic_ms() + timeout_ms;
    for (;;) {
        int ready = take_line(qmp, message, capacity);
        if (ready < 0) {
            qmp_fail(error, EMU_ERR_ENGINE,
                     "QMP message exceeds output buffer");
            return -1;
        }
        if (ready) {
            if (!valid_json_shape(message)) {
                qmp_fail(error, EMU_ERR_ENGINE,
                         "malformed or excessively nested QMP message");
                return -1;
            }
            qmp_ok(error);
            return 1;
        }
        if (qmp->input_size == sizeof qmp->input) {
            qmp_fail(error, EMU_ERR_ENGINE,
                     "unterminated QMP message exceeds %u bytes",
                     EMU_QMP_MESSAGE_MAX);
            return -1;
        }
        uint64_t now = monotonic_ms();
        int remaining = now >= deadline ? 0 : (int)(deadline - now);
        struct pollfd descriptor = {.fd = qmp->fd, .events = POLLIN};
        int status;
        do {
            status = poll(&descriptor, 1, remaining);
        } while (status < 0 &&
                 (errno == EINTR || errno == EAGAIN));
        if (!status) {
            qmp_ok(error);
            return 0;
        }
        if (status < 0 || (descriptor.revents & POLLNVAL)) {
            qmp_fail(error, EMU_ERR_IO, "QMP poll failed: %s",
                     strerror(status < 0 ? errno : EBADF));
            return -1;
        }
        /* POLLERR/POLLHUP can accompany readable final replies during quit.
         * Drain those bytes first; read reports a reset/EOF once exhausted. */
        ssize_t count;
        do {
            count = read(qmp->fd, qmp->input + qmp->input_size,
                         sizeof qmp->input - qmp->input_size);
        } while (count < 0 && errno == EINTR);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count < 0) {
            qmp_fail(error, EMU_ERR_IO, "cannot read QMP: %s",
                     strerror(errno));
            return -1;
        }
        if (!count) {
            qmp_fail(error, EMU_ERR_ENGINE, "QMP connection closed");
            return -1;
        }
        qmp->input_size += (size_t)count;
    }
}

static int json_uint(const char *json, const char *name, uint64_t *value) {
    char key[64];
    if (snprintf(key, sizeof key, "\"%s\"", name) < 0) return 0;
    const char *cursor = strstr(json, key);
    if (!cursor) return 0;
    cursor += strlen(key);
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (*cursor++ != ':') return 0;
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (*cursor < '0' || *cursor > '9') return 0;
    uint64_t result = 0;
    do {
        unsigned digit = (unsigned)(*cursor++ - '0');
        if (result > (UINT64_MAX - digit) / 10u) return 0;
        result = result * 10u + digit;
    } while (*cursor >= '0' && *cursor <= '9');
    *value = result;
    return 1;
}

static void qmp_version(const char *greeting, char output[], size_t capacity) {
    uint64_t major = 0, minor = 0, micro = 0;
    if (json_uint(greeting, "major", &major) &&
        json_uint(greeting, "minor", &minor) &&
        json_uint(greeting, "micro", &micro))
        snprintf(output, capacity, "%llu.%llu.%llu",
                 (unsigned long long)major, (unsigned long long)minor,
                 (unsigned long long)micro);
}

static int response_id(const char *message, uint64_t *id) {
    return json_uint(message, "id", id);
}

static void json_string(const char *message, const char *name,
                         char output[], size_t capacity) {
    const char *key = strstr(message, name);
    if (!key || !(key = strchr(key, ':'))) return;
    key++;
    while (*key == ' ' || *key == '\t') key++;
    if (*key++ != '\"') return;
    size_t used = 0;
    while (*key && *key != '\"' && used + 1u < capacity) {
        if (*key == '\\' && key[1]) key++;
        output[used++] = *key++;
    }
    output[used] = 0;
}

emu_error_code_t emu_qmp_command(emu_qmp_t *qmp, const char *command,
                                 const char *arguments_json, int send_fd,
                                 char *response, size_t response_capacity,
                                 unsigned timeout_ms, emu_error_t *error) {
    if (!qmp || qmp->fd < 0 || !valid_command(command) ||
        (response && !response_capacity))
        return qmp_fail(error, EMU_ERR_ARGUMENT,
                        "invalid QMP command request");
    uint64_t id = qmp->next_id++;
    char request[4096];
    int length = arguments_json
        ? snprintf(request, sizeof request,
                   "{\"execute\":\"%s\",\"arguments\":%s,\"id\":%llu}\r\n",
                   command, arguments_json, (unsigned long long)id)
        : snprintf(request, sizeof request,
                   "{\"execute\":\"%s\",\"id\":%llu}\r\n",
                   command, (unsigned long long)id);
    if (length < 0 || (size_t)length >= sizeof request)
        return qmp_fail(error, EMU_ERR_ARGUMENT,
                        "QMP command is too large");
    if (send_bytes(qmp->fd, request, (size_t)length, send_fd))
        return qmp_fail(error, EMU_ERR_IO,
                        "cannot send QMP command: %s", strerror(errno));
    char message[EMU_QMP_MESSAGE_MAX];
    for (;;) {
        int ready = emu_qmp_next_message(qmp, message, sizeof message,
                                         timeout_ms, error);
        if (ready < 0) return error ? error->code : EMU_ERR_IO;
        if (!ready)
            return qmp_fail(error, EMU_ERR_ENGINE,
                            "QMP command %s timed out", command);
        uint64_t received;
        if (!response_id(message, &received) || received != id) continue;
        if (strstr(message, "\"error\"")) {
            char description[160] = "QEMU rejected the command";
            json_string(message, "\"desc\"", description, sizeof description);
            return qmp_fail(error, EMU_ERR_ENGINE, "QMP %s: %s",
                            command, description);
        }
        if (!strstr(message, "\"return\""))
            return qmp_fail(error, EMU_ERR_ENGINE,
                            "QMP %s returned an invalid response", command);
        if (response) {
            size_t size = strlen(message);
            if (size + 1u > response_capacity)
                return qmp_fail(error, EMU_ERR_ENGINE,
                                "QMP response exceeds output buffer");
            memcpy(response, message, size + 1u);
        }
        qmp_ok(error);
        return EMU_OK;
    }
}

emu_error_code_t emu_qmp_handshake(emu_qmp_t *qmp, unsigned timeout_ms,
                                   emu_error_t *error) {
    char greeting[EMU_QMP_MESSAGE_MAX];
    int ready = emu_qmp_next_message(qmp, greeting, sizeof greeting,
                                     timeout_ms, error);
    if (ready < 0) return error ? error->code : EMU_ERR_IO;
    if (!ready)
        return qmp_fail(error, EMU_ERR_ENGINE, "QMP greeting timed out");
    if (!strstr(greeting, "\"QMP\"") ||
        !strstr(greeting, "\"version\""))
        return qmp_fail(error, EMU_ERR_ENGINE, "invalid QMP greeting");
    qmp_version(greeting, qmp->version, sizeof qmp->version);
    json_string(greeting, "\"package\"", qmp->build, sizeof qmp->build);
    if (!qmp->version[0])
        return qmp_fail(error, EMU_ERR_ENGINE,
                        "QMP greeting has no semantic version");
    return emu_qmp_command(qmp, "qmp_capabilities", NULL, -1,
                           NULL, 0, timeout_ms, error);
}
