#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif

#include "emu_gdb.h"
#include "gdb_binary.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

static int fail(const char *operation) {
    fprintf(stderr, "error: GDB %s: %s\n", operation, strerror(errno));
    return -1;
}

static _Noreturn void child_fail(int fd) {
    int error = errno;
    ssize_t result;
    do { result = write(fd, &error, sizeof error); }
    while (result < 0 && errno == EINTR);
    _exit(127);
}

static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    return flags < 0 ? -1 : fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int socketpair_cloexec(int pair[2]) {
#ifdef SOCK_CLOEXEC
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0)
        return 0;
    if (errno != EINVAL) return -1;
#endif
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return -1;
    if (set_cloexec(pair[0]) || set_cloexec(pair[1])) {
        int saved_errno = errno;
        close(pair[0]);
        close(pair[1]);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static int pipe_cloexec(int pair[2]) {
#if defined(__linux__)
    if (pipe2(pair, O_CLOEXEC) == 0) return 0;
    if (errno != ENOSYS && errno != EINVAL) return -1;
#endif
    if (pipe(pair)) return -1;
    if (set_cloexec(pair[0]) || set_cloexec(pair[1])) {
        int saved_errno = errno;
        close(pair[0]);
        close(pair[1]);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static int close_inherited_descriptors(void) {
#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, 4u, ~0u, 0u) == 0) return 0;
#endif
    long maximum = sysconf(_SC_OPEN_MAX);
    if (maximum < 0) {
        struct rlimit limit;
        if (getrlimit(RLIMIT_NOFILE, &limit)) return -1;
        if (limit.rlim_cur == RLIM_INFINITY) {
            errno = EOVERFLOW;
            return -1;
        }
        maximum = limit.rlim_cur > INT_MAX ? INT_MAX : (long)limit.rlim_cur;
    }
    if (maximum > INT_MAX) maximum = INT_MAX;
    for (int fd = 4; fd < maximum; fd++) close(fd);
    return 0;
}

static ssize_t send_without_sigpipe(int fd, const void *bytes, size_t size) {
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                   &enabled, sizeof enabled)) return -1;
#endif
    struct iovec iov = {.iov_base = (void *)bytes, .iov_len = size};
    struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1};
#ifdef MSG_NOSIGNAL
    return sendmsg(fd, &message, MSG_NOSIGNAL);
#else
    return sendmsg(fd, &message, 0);
#endif
}

/* EMU is in the background while GDB owns the terminal. Block SIGTTOU only
 * around the ownership/termios operations, preserving the caller's policy. */
static int terminal_set(emu_gdb_t *g, pid_t group, const struct termios *settings) {
    if (!g->terminal) return 0;
    sigset_t block, saved;
    sigemptyset(&block);
    sigaddset(&block, SIGTTOU);
    if (sigprocmask(SIG_BLOCK, &block, &saved)) return fail("terminal signal mask");
    int rc = tcsetpgrp(STDIN_FILENO, group);
    if (!rc && settings) rc = tcsetattr(STDIN_FILENO, TCSANOW, settings);
    int saved_errno = errno;
    if (sigprocmask(SIG_SETMASK, &saved, NULL)) return fail("restore signal mask");
    errno = saved_errno;
    return rc ? fail("terminal ownership/settings") : 0;
}

int emu_gdb_prepare(emu_gdb_t *g, const emu_cli_options_t *cli,
                    emu_runtime_options_t *options) {
    const char *binary = cli->gdb_binary ? cli->gdb_binary : EMU_GDB_BINARY;
    struct stat st;
    if (stat(binary, &st) || !S_ISREG(st.st_mode) || access(binary, X_OK)) {
        fprintf(stderr, "error: GDB binary is not executable: %s\n"
                        "build it with: make -C emu gdb\n", binary);
        return -1;
    }
    if (snprintf(options->gdb_binary, sizeof options->gdb_binary, "%s", binary)
            >= (int)sizeof options->gdb_binary) {
        errno = ENAMETOOLONG;
        return fail("binary path");
    }
    options->gdb_enabled = 1;
    options->gdb_mode = cli->dbg_interactive ? 2 : 1;
    options->gdb_action_count = cli->gdb_action_count;
    memcpy(options->gdb_actions, cli->gdb_actions,
           cli->gdb_action_count * sizeof cli->gdb_actions[0]);
    if (!cli->gdb_enabled) {
        strcpy(g->directory, "/tmp/emu-gdb-XXXXXX");
        if (!mkdtemp(g->directory)) {
            g->directory[0] = 0;
            return fail("temporary directory");
        }
        snprintf(options->gdb_socket, sizeof options->gdb_socket,
                 "%s/remote", g->directory);
    }
    return 0;
}

int emu_gdb_start(emu_gdb_t *g, const emu_runtime_options_t *options) {
    char connect[160];
    if (options->gdb_socket[0])
        snprintf(connect, sizeof connect, "target remote %s", options->gdb_socket);
    else
        snprintf(connect, sizeof connect, "target remote 127.0.0.1:%u", options->gdb_port);
    char *arguments[16 + 2 * EMU_GDB_MAX_ACTIONS];
    size_t count = 0;
    arguments[count++] = (char *)options->gdb_binary;
    arguments[count++] = "-nx";
    arguments[count++] = "-nh";
    arguments[count++] = "-q";
    arguments[count++] = "-iex";
    arguments[count++] = "set auto-load off";
    if (options->gdb_mode == 1) arguments[count++] = "--batch";
    arguments[count++] = "-ex";
    arguments[count++] = connect;
    for (size_t i = 0; i < options->gdb_action_count; i++) {
        arguments[count++] = options->gdb_actions[i].script ? "-x" : "-ex";
        arguments[count++] = (char *)options->gdb_actions[i].value;
    }
    arguments[count] = NULL;

    /* Non-terminal stdin is useful for scripted interactive clients too. A
     * terminal without a controlling session simply retains inherited streams. */
    g->foreground = tcgetpgrp(STDIN_FILENO);
    if (options->gdb_mode == 2 && g->foreground >= 0) {
        if (g->foreground != getpgrp()) {
            errno = EPERM;
            return fail("launch requires foreground terminal");
        }
        if (tcgetattr(STDIN_FILENO, &g->original_termios)) return fail("read termios");
        g->terminal = 1;
    }
    int gate[2], exec_error[2];
    if (socketpair_cloexec(gate))
        return fail("launch gate");
    if (pipe_cloexec(exec_error)) {
        close(gate[0]);
        close(gate[1]);
        return fail("exec status pipe");
    }
    fflush(NULL);
    pid_t child = fork();
    if (!child) {
        close(gate[1]);
        close(exec_error[0]);
        if (setpgid(0, 0)) child_fail(exec_error[1]);
        /* Do not inherit EMU's signal handlers or blocked job-control signals. */
        const int signals[] = {SIGINT, SIGTERM, SIGQUIT, SIGTSTP, SIGTTIN,
                               SIGTTOU, SIGCONT, SIGPIPE, SIGCHLD};
        for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++)
            signal(signals[i], SIG_DFL);
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);
        char ready;
        ssize_t n;
        do { n = read(gate[0], &ready, 1); } while (n < 0 && errno == EINTR);
        if (n != 1) { errno = EIO; child_fail(exec_error[1]); }
        close(gate[0]);
        if (exec_error[1] != 3 && dup2(exec_error[1], 3) < 0)
            child_fail(exec_error[1]);
        if (set_cloexec(3)) child_fail(3);
        /* No host transport or artifact descriptors may survive into GDB. */
        if (close_inherited_descriptors()) child_fail(3);
        execv(options->gdb_binary, arguments);
        child_fail(3);
    }
    close(gate[0]);
    close(exec_error[1]);
    if (child < 0) {
        close(gate[1]);
        close(exec_error[0]);
        return fail("fork");
    }
    g->child = child;
    if (setpgid(child, child) || terminal_set(g, child, NULL)) {
        close(gate[1]);
        close(exec_error[0]);
        return fail("process group");
    }
    /* Child cannot read the terminal before foreground ownership is installed. */
    ssize_t n = send_without_sigpipe(gate[1], "x", 1);
    close(gate[1]);
    if (n != 1) {
        close(exec_error[0]);
        return fail("release launch gate");
    }
    int error = 0;
    do { n = read(exec_error[0], &error, sizeof error); }
    while (n < 0 && errno == EINTR);
    close(exec_error[0]);
    if (n > 0) { errno = error; return fail("exec"); }
    return n < 0 ? fail("read exec status") : 0;
}

int emu_gdb_poll(emu_gdb_t *g) {
    if (!g->child || g->exited) return g->exited;
    int status;
    pid_t result = waitpid(g->child, &status, WNOHANG | WUNTRACED);
    if (result < 0) return errno == EINTR ? 0 : fail("waitpid");
    if (!result) return 0;
    if (WIFSTOPPED(status)) {
        if (!g->terminal) {
            errno = EIO;
            return fail("child stopped without interactive terminal");
        }
        struct termios settings;
        if (tcgetattr(STDIN_FILENO, &settings)) return fail("read suspended termios");
        if (terminal_set(g, g->foreground, &g->original_termios)) return -1;
        /* Let the shell manage EMU as the job. SIGSTOP works for orphaned
         * controlling-terminal sessions as well as ordinary shell jobs. */
        raise(SIGSTOP);
        /* A shell's `bg` must not let us steal its terminal. Wait for `fg`. */
        while (tcgetpgrp(STDIN_FILENO) != g->foreground) {
            if (emu_runtime_stop_signal()) return 0;
            raise(SIGSTOP);
        }
        if (terminal_set(g, g->child, &settings)) return -1;
        if (kill(-g->child, SIGCONT)) return fail("resume");
        return 0;
    }
    g->exited = 1;
    g->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return terminal_set(g, g->foreground, &g->original_termios) ? -1 : 1;
}

int emu_gdb_interrupt(emu_gdb_t *g) {
    return g->child && !g->exited && kill(-g->child, SIGINT) && errno != ESRCH
        ? fail("interrupt") : 0;
}

int emu_gdb_stop(emu_gdb_t *g) {
    int rc = 0;
    if (g->child && !g->exited) {
        kill(-g->child, SIGTERM);
        kill(-g->child, SIGCONT);
        int status;
        pid_t result = 0;
        for (unsigned i = 0; i < 50 && result == 0; i++) {
            result = waitpid(g->child, &status, WNOHANG);
            if (result < 0 && errno == EINTR) result = 0;
            if (!result) {
                struct timespec delay = {0, 10000000};
                nanosleep(&delay, NULL);
            }
        }
        if (!result) {
            kill(-g->child, SIGKILL);
            do { result = waitpid(g->child, &status, 0); }
            while (result < 0 && errno == EINTR);
        }
        if (result < 0) rc = fail("reap");
        else {
            g->exited = 1;
            g->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
    }
    if (terminal_set(g, g->foreground, &g->original_termios)) rc = -1;
    g->terminal = 0;
    return rc;
}

int emu_gdb_cleanup(emu_gdb_t *g) {
    int rc = emu_gdb_stop(g);
    if (g->directory[0]) {
        char socket[108];
        snprintf(socket, sizeof socket, "%s/remote", g->directory);
        if (unlink(socket) && errno != ENOENT) rc = fail("remove socket");
        if (rmdir(g->directory)) rc = fail("remove directory");
        g->directory[0] = 0;
    }
    return rc;
}
