#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_support.h"
#include "emu_qmp.h"

static void write_text(int fd, const char *text) {
    size_t size = strlen(text);
    while (size) {
        ssize_t count = write(fd, text, size);
        if (count < 0 && errno == EINTR) continue;
        EMU_CHECK(count > 0);
        text += count;
        size -= (size_t)count;
    }
}

static void read_request(int fd, char *output, size_t capacity,
                         int *received_fd) {
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec iov = {.iov_base = output, .iov_len = capacity - 1u};
    struct msghdr message = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof control,
    };
    ssize_t count;
    do {
        count = recvmsg(fd, &message, 0);
    } while (count < 0 && errno == EINTR);
    EMU_CHECK(count > 0);
    output[count] = 0;
    if (received_fd) {
        *received_fd = -1;
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        if (header && header->cmsg_level == SOL_SOCKET &&
            header->cmsg_type == SCM_RIGHTS)
            memcpy(received_fd, CMSG_DATA(header), sizeof *received_fd);
    }
}

static void fake_qmp(int fd) {
    char request[4096];
    write_text(fd, "{\"QMP\":{\"version\":{\"qemu\":{\"micro\":50,"
                   "\"minor\":1,\"major\":11},\"package\":\"test\"},"
                   "\"capabilities\":[]}}\r\n");
    read_request(fd, request, sizeof request, NULL);
    EMU_CHECK(strstr(request, "qmp_capabilities") != NULL);
    write_text(fd, "{\"return\":{},\"id\":1}\r\n");
    read_request(fd, request, sizeof request, NULL);
    EMU_CHECK(strstr(request, "query-status") != NULL);
    write_text(fd, "{\"event\":\"STOP\",\"data\":{}}\r\n");
    write_text(fd, "{\"return\":{\"status\":\"paused\"},\"id\":2}\r\n");
    int received_fd = -1;
    read_request(fd, request, sizeof request, &received_fd);
    EMU_CHECK(strstr(request, "getfd") != NULL && received_fd >= 0);
    close(received_fd);
    write_text(fd, "{\"return\":{},\"id\":3}\r\n");
    read_request(fd, request, sizeof request, NULL);
    EMU_CHECK(strstr(request, "bad-command") != NULL);
    write_text(fd, "{\"error\":{\"class\":\"CommandNotFound\","
                   "\"desc\":\"not available\"},\"id\":4}\r\n");
    close(fd);
    _exit(0);
}

static void test_handshake_commands_and_fd(void) {
    int sockets[2];
    int pipefd[2];
    EMU_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    EMU_CHECK(pipe(pipefd) == 0);
    pid_t child = fork();
    EMU_CHECK(child >= 0);
    if (!child) {
        close(sockets[0]);
        close(pipefd[0]);
        close(pipefd[1]);
        fake_qmp(sockets[1]);
    }
    close(sockets[1]);
    emu_qmp_t qmp;
    emu_error_t error = {0};
    char response[1024];
    emu_qmp_init(&qmp, sockets[0]);
    EMU_CHECK(emu_qmp_handshake(&qmp, 1000, &error) == EMU_OK);
    EMU_CHECK(!strcmp(qmp.version, "11.1.50"));
    EMU_CHECK(emu_qmp_command(&qmp, "query-status", NULL, -1,
                              response, sizeof response, 1000, &error) ==
              EMU_OK);
    EMU_CHECK(strstr(response, "paused") != NULL);
    EMU_CHECK(emu_qmp_command(&qmp, "getfd",
                              "{\"fdname\":\"display\"}", pipefd[0],
                              NULL, 0, 1000, &error) == EMU_OK);
    EMU_CHECK(emu_qmp_command(&qmp, "bad-command", NULL, -1,
                              NULL, 0, 1000, &error) == EMU_ERR_ENGINE);
    EMU_CHECK(strstr(error.message, "not available") != NULL);
    close(pipefd[0]);
    close(pipefd[1]);
    close(sockets[0]);
    int status = 0;
    EMU_CHECK(waitpid(child, &status, 0) == child);
    EMU_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_rejection_and_timeout(void) {
    int sockets[2];
    EMU_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    emu_qmp_t qmp;
    emu_error_t error = {0};
    emu_qmp_init(&qmp, sockets[0]);
    write_text(sockets[1], "{}\r\n");
    EMU_CHECK(emu_qmp_handshake(&qmp, 100, &error) == EMU_ERR_ENGINE);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_fragmented_and_nested_rejection(void) {
    int sockets[2];
    EMU_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    emu_qmp_t qmp;
    emu_error_t error = {0};
    char message[128];
    emu_qmp_init(&qmp, sockets[0]);
    pid_t child = fork();
    EMU_CHECK(child >= 0);
    if (!child) {
        close(sockets[0]);
        write_text(sockets[1], "{\"return\":");
        struct timespec pause = {.tv_nsec = 1000000};
        nanosleep(&pause, NULL);
        write_text(sockets[1], "{},\"id\":7}\r\n");
        close(sockets[1]);
        _exit(0);
    }
    close(sockets[1]);
    EMU_CHECK(emu_qmp_next_message(
        &qmp, message, sizeof message, 1000, &error) == 1);
    EMU_CHECK(strstr(message, "\"id\":7") != NULL);
    close(sockets[0]);
    EMU_CHECK(waitpid(child, NULL, 0) == child);

    EMU_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    emu_qmp_init(&qmp, sockets[0]);
    char nested[EMU_QMP_NESTING_MAX + 8u];
    size_t used = 0;
    nested[used++] = '{';
    for (unsigned i = 0; i < EMU_QMP_NESTING_MAX; i++) nested[used++] = '[';
    nested[used++] = '\n';
    nested[used] = 0;
    write_text(sockets[1], nested);
    EMU_CHECK(emu_qmp_next_message(
        &qmp, message, sizeof message, 100, &error) == -1);
    EMU_CHECK(error.code == EMU_ERR_ENGINE);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_final_reply_before_reset(void) {
    int sockets[2];
    EMU_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    emu_qmp_t qmp;
    emu_error_t error = {0};
    char message[128];
    emu_qmp_init(&qmp, sockets[0]);
    /* Closing with unread inbound data reports POLLERR together with POLLIN.
     * A final response already queued by the peer must still be consumed. */
    write_text(sockets[0], "{\"execute\":\"quit\",\"id\":1}\n");
    write_text(sockets[1], "{\"return\":{},\"id\":1}\n");
    close(sockets[1]);
    EMU_CHECK(emu_qmp_next_message(&qmp, message, sizeof message, 100, &error) == 1);
    EMU_CHECK(strstr(message, "\"id\":1") != NULL);
    EMU_CHECK(emu_qmp_next_message(&qmp, message, sizeof message, 100, &error) == -1);
    close(sockets[0]);
}

static void test_closed_peer_reports_error_without_sigpipe(void) {
    int sockets[2];
    int payload[2];
    EMU_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    EMU_CHECK(pipe(payload) == 0);
    close(sockets[1]);
    emu_qmp_t qmp;
    emu_error_t error = {0};
    emu_qmp_init(&qmp, sockets[0]);
    EMU_CHECK(emu_qmp_command(&qmp, "query-status", NULL, -1,
                              NULL, 0, 100, &error) == EMU_ERR_IO);
    EMU_CHECK(strstr(error.message, "cannot send QMP command") != NULL);
    EMU_CHECK(emu_qmp_command(&qmp, "getfd", NULL, payload[0],
                              NULL, 0, 100, &error) == EMU_ERR_IO);
    close(payload[0]);
    close(payload[1]);
    close(sockets[0]);
}

int main(void) {
    test_handshake_commands_and_fd();
    test_rejection_and_timeout();
    test_fragmented_and_nested_rejection();
    test_final_reply_before_reset();
    test_closed_peer_reports_error_without_sigpipe();
    puts("QMP transport: PASS");
    return 0;
}
