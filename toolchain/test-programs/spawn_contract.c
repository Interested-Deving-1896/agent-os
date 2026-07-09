#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __wasi__
#include <wasi/api.h>
#endif

extern char **environ;

static volatile sig_atomic_t usr1_calls;
static volatile sig_atomic_t masked_usr1_calls;
static volatile sig_atomic_t masked_usr1_depth;
static volatile sig_atomic_t masked_usr1_max_depth;
static volatile sig_atomic_t usr2_calls;
static volatile sig_atomic_t usr2_during_usr1;

static void usr1_handler(int signal) {
    (void)signal;
    usr1_calls++;
}

static void usr2_handler(int signal) {
    (void)signal;
    usr2_calls++;
    if (masked_usr1_depth != 0)
        usr2_during_usr1 = 1;
}

static void masked_usr1_handler(int signal) {
    (void)signal;
    masked_usr1_depth++;
    if (masked_usr1_depth > masked_usr1_max_depth)
        masked_usr1_max_depth = masked_usr1_depth;
    masked_usr1_calls++;
    if (masked_usr1_calls == 1) {
        kill(getpid(), SIGUSR2);
        kill(getpid(), SIGUSR1);
    }
    masked_usr1_depth--;
}

static void reset_usr1_handler(int signal) {
    (void)signal;
}

static int wait_exit(pid_t child, int expected) {
    int status = 0;
    return waitpid(child, &status, 0) == child &&
        WIFEXITED(status) && WEXITSTATUS(status) == expected;
}

static int spawn_self(const char *self, posix_spawn_file_actions_t *actions,
    posix_spawnattr_t *attributes, char *const child_argv[], int *status_out) {
    pid_t child = -1;
    int error = posix_spawnp(&child, self, actions, attributes,
        child_argv, environ);
    if (error != 0)
        return error;
    int status = 0;
    if (waitpid(child, &status, 0) != child)
        return errno;
    *status_out = status;
    return 0;
}

static int query_mask_member(int signal) {
    sigset_t current;
    if (sigprocmask(SIG_SETMASK, NULL, &current) != 0)
        return -1;
    return sigismember(&current, signal);
}

static int child_actions(int argc, char **argv) {
    if (argc != 7)
        return 20;
    int dir_fd = atoi(argv[3]);
    int closed_fd = atoi(argv[4]);
    int readable_fd = atoi(argv[5]);
    int inherited_fd = atoi(argv[6]);
    char cwd[512];
    char payload[8] = {0};
    struct stat payload_stat;
    errno = 0;
    int dir_closed = fcntl(dir_fd, F_GETFD) == -1 && errno == EBADF;
    errno = 0;
    int source_closed = fcntl(closed_fd, F_GETFD) == -1 && errno == EBADF;
    errno = 0;
    int inherited_closed =
        fcntl(inherited_fd, F_GETFD) == -1 && errno == EBADF;
    int cwd_ok = getcwd(cwd, sizeof(cwd)) != NULL && strcmp(cwd, argv[2]) == 0;
    int readable = read(readable_fd, payload, sizeof(payload)) == 6 &&
        memcmp(payload, "action", 6) == 0;
    int cloexec_cleared = (fcntl(readable_fd, F_GETFD) & FD_CLOEXEC) == 0;
    int public_preopen_hidden;
#ifdef __wasi__
    __wasi_prestat_t prestat;
    public_preopen_hidden =
        __wasi_fd_prestat_get(3, &prestat) == __WASI_ERRNO_BADF;
#else
    errno = 0;
    public_preopen_hidden = fcntl(3, F_GETFD) == -1 && errno == EBADF;
#endif
    int path_resolution_preserved = stat("payload", &payload_stat) == 0 &&
        S_ISREG(payload_stat.st_mode);
    int ok = dir_closed && source_closed && inherited_closed && cwd_ok &&
        readable && cloexec_cleared && public_preopen_hidden &&
        path_resolution_preserved;
    if (!ok)
        fprintf(stderr,
            "spawn_actions detail: dir_closed=%d source_closed=%d inherited_closed=%d cwd_ok=%d readable=%d cloexec_cleared=%d public_preopen_hidden=%d path_resolution_preserved=%d errno=%d cwd=%s expected=%s payload=%.*s\n",
            dir_closed, source_closed, inherited_closed, cwd_ok, readable,
            cloexec_cleared, public_preopen_hidden, path_resolution_preserved,
            errno, cwd, argv[2], (int)sizeof(payload), payload);
    printf("spawn_closefrom_public_preopen_hidden=%s\n",
        public_preopen_hidden && path_resolution_preserved ? "yes" : "no");
    printf("spawn_actions_child=%s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 21;
}

static int child_inherit(int argc, char **argv) {
    if (argc != 4)
        return 30;
    int shared_fd = atoi(argv[2]);
    int cloexec_fd = atoi(argv[3]);
    char byte = 0;
    int shared = read(shared_fd, &byte, 1) == 1 && byte == 'b';
    errno = 0;
    int cloexec_closed = fcntl(cloexec_fd, F_GETFD) == -1 && errno == EBADF;
    int ok = shared && cloexec_closed;
    if (!ok)
        fprintf(stderr,
            "spawn_inherit detail: shared=%d byte=%d cloexec_closed=%d errno=%d shared_fd=%d cloexec_fd=%d\n",
            shared, (int)(unsigned char)byte, cloexec_closed, errno,
            shared_fd, cloexec_fd);
    printf("spawn_inherit_child=%s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 31;
}

static int child_same_fd_dup2(int argc, char **argv) {
    if (argc != 3)
        return 32;
    int fd = atoi(argv[2]);
    int flags = fcntl(fd, F_GETFD);
    int ok = flags >= 0 && (flags & FD_CLOEXEC) == 0;
    if (!ok)
        fprintf(stderr,
            "spawn same-fd dup2: fd=%d flags=%d errno=%d\n",
            fd, flags, errno);
    return ok ? 0 : 33;
}

static int child_live_cwd(int argc, char **argv) {
    char cwd[512] = {0};
    int ok = argc == 3 && getcwd(cwd, sizeof(cwd)) != NULL &&
        strcmp(cwd, argv[2]) == 0;
    if (!ok)
        fprintf(stderr, "spawn live cwd: cwd=%s expected=%s errno=%d\n",
            cwd, argc == 3 ? argv[2] : "<missing>", errno);
    printf("spawn_live_cwd_child=%s\n", ok ? "yes" : "no");
    return ok ? 0 : 34;
}

static int child_mask_query(int argc, char **argv) {
    if (argc != 3)
        return 40;
    int expected = strcmp(argv[2], "blocked") == 0;
    int observed = query_mask_member(SIGUSR1);
    int ok = observed == expected;
    printf("spawn_mask_child=%s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 41;
}

static int child_mask_delivery(void) {
    struct sigaction action;
    sigset_t usr1;
    memset(&action, 0, sizeof(action));
    action.sa_handler = usr1_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0 ||
        sigemptyset(&usr1) != 0 || sigaddset(&usr1, SIGUSR1) != 0)
        return 50;
    int initially_blocked = query_mask_member(SIGUSR1) == 1;
    if (kill(getpid(), SIGUSR1) != 0 || kill(getpid(), SIGUSR1) != 0)
        return 51;
    usleep(1000);
    int delayed = usr1_calls == 0;
    if (sigprocmask(SIG_UNBLOCK, &usr1, NULL) != 0)
        return 52;
    usleep(1000);
    int coalesced = usr1_calls == 1;
    int ok = initially_blocked && delayed && coalesced;
    printf("spawn_mask_delivery=%s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 53;
}

static int child_sigpipe(void) {
    if (kill(getpid(), SIGPIPE) != 0)
        return 60;
    usleep(1000);
    printf("spawn_sigpipe_ignored=yes\n");
    return 0;
}

static int child_resethand(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = reset_usr1_handler;
    action.sa_flags = SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0 ||
        kill(getpid(), SIGUSR1) != 0)
        return 61;
    usleep(1000);
    if (kill(getpid(), SIGUSR1) != 0)
        return 62;
    usleep(1000);
    return 63;
}

static int child_pgroup(int argc, char **argv) {
    pid_t expected = argc == 3 ? (pid_t)atoi(argv[2]) : getpid();
    pid_t observed_pgrp = getpgrp();
    pid_t observed_pgid = getpgid(0);
    int ok = observed_pgrp == expected && observed_pgid == expected;
    if (!ok)
        fprintf(stderr, "spawn pgroup: expected=%d getpgrp=%d getpgid=%d errno=%d\n",
            (int)expected, (int)observed_pgrp, (int)observed_pgid, errno);
    printf("spawn_pgroup_child=%s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 70;
}

static ssize_t read_line_fd(int fd, char *buffer, size_t capacity) {
    size_t used = 0;
    while (used + 1 < capacity) {
        ssize_t result = read(fd, buffer + used, 1);
        if (result <= 0)
            return used == 0 ? result : (ssize_t)used;
        if (buffer[used++] == '\n')
            break;
    }
    buffer[used] = '\0';
    return (ssize_t)used;
}

static int child_bidirectional(void) {
    struct stat stdin_stat;
    struct stat stdout_stat;
    if (fstat(STDIN_FILENO, &stdin_stat) != 0 ||
        fstat(STDOUT_FILENO, &stdout_stat) != 0 ||
        !S_ISFIFO(stdin_stat.st_mode) || !S_ISFIFO(stdout_stat.st_mode)) {
        fprintf(stderr,
            "spawn bidirectional fstat: stdin=%#o stdout=%#o errno=%d\n",
            (unsigned)stdin_stat.st_mode, (unsigned)stdout_stat.st_mode, errno);
        return 85;
    }
    char request[32];
    const char *expected[] = {"request-one\n", "request-two\n"};
    const char *responses[] = {"response-one\n", "response-two\n"};
    for (size_t index = 0; index < 2; index++) {
        ssize_t length = read_line_fd(STDIN_FILENO, request, sizeof(request));
        if (length != (ssize_t)strlen(expected[index]) ||
            memcmp(request, expected[index],
                length > 0 ? (size_t)length : 0) != 0) {
            fprintf(stderr,
                "spawn bidirectional child read: index=%zu length=%zd expected=%zu errno=%d\n",
                index, length, strlen(expected[index]), errno);
            return 80 + (int)index;
        }
        if (write(STDOUT_FILENO, responses[index], strlen(responses[index])) !=
            (ssize_t)strlen(responses[index]))
            return 82 + (int)index;
    }
    char eof_probe;
    errno = 0;
    ssize_t eof_read = read(STDIN_FILENO, &eof_probe, 1);
    if (eof_read != 0) {
        fprintf(stderr,
            "spawn bidirectional child EOF: length=%zd errno=%d\n",
            eof_read, errno);
        return 84;
    }
    return 0;
}

static int child_waitpid_producer(void) {
    const char payload[] = "sibling-ready\n";
    return write(STDOUT_FILENO, payload, sizeof(payload) - 1) ==
        (ssize_t)(sizeof(payload) - 1) ? 0 : 87;
}

static int child_waitpid_consumer(void) {
    char payload[32];
    ssize_t length = read_line_fd(STDIN_FILENO, payload, sizeof(payload));
    return length == 14 && strcmp(payload, "sibling-ready\n") == 0 ? 0 : 88;
}

static int child_hold_stdin_until_eof(void) {
    char buffer[32];
    while (read(STDIN_FILENO, buffer, sizeof(buffer)) > 0) {
    }
    return 0;
}

static int test_spawn_pipe_writer_lifetime(const char *self,
        int close_parent_writer_before_holder, int holder_uses_closefrom) {
    int output[2] = {-1, -1};
    int control[2] = {-1, -1};
    if (pipe(output) != 0 || pipe(control) != 0)
        return 0;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    int producer_setup =
        posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO) == 0 &&
        posix_spawn_file_actions_addclose(&actions, output[0]) == 0 &&
        posix_spawn_file_actions_addclose(&actions, output[1]) == 0 &&
        posix_spawn_file_actions_addclose(&actions, control[0]) == 0 &&
        posix_spawn_file_actions_addclose(&actions, control[1]) == 0;
    char *producer_argv[] = {(char *)self, "--waitpid-producer", NULL};
    pid_t producer = -1;
    int producer_error = producer_setup
        ? posix_spawnp(&producer, self, &actions, NULL, producer_argv, environ)
        : EINVAL;
    posix_spawn_file_actions_destroy(&actions);

    if (close_parent_writer_before_holder) {
        if (close(output[1]) != 0)
            return 0;
        output[1] = -1;
    }

    posix_spawn_file_actions_init(&actions);
    int holder_setup =
        posix_spawn_file_actions_adddup2(&actions, control[0], STDIN_FILENO) == 0 &&
        posix_spawn_file_actions_addclose(&actions, control[0]) == 0 &&
        posix_spawn_file_actions_addclose(&actions, control[1]) == 0;
    if (holder_setup && holder_uses_closefrom)
        holder_setup = posix_spawn_file_actions_addclosefrom_np(&actions,
            STDERR_FILENO + 1) == 0;
    char *holder_argv[] = {(char *)self, "--hold-stdin", NULL};
    pid_t holder = -1;
    int holder_error = producer_error == 0 && holder_setup
        ? posix_spawnp(&holder, self, &actions, NULL, holder_argv, environ)
        : EINVAL;
    posix_spawn_file_actions_destroy(&actions);

    if (output[1] >= 0)
        close(output[1]);
    close(control[0]);
    char payload[32] = {0};
    ssize_t payload_length = read_line_fd(output[0], payload, sizeof(payload));
    int producer_status = 0;
    pid_t producer_wait = producer_error == 0
        ? waitpid(producer, &producer_status, 0) : -1;
    struct pollfd eof_poll = {
        .fd = output[0],
        .events = POLLIN | POLLHUP,
        .revents = 0,
    };
    int poll_result = poll(&eof_poll, 1, 1000);
    char eof_byte = 0;
    ssize_t eof_read = poll_result > 0 ? read(output[0], &eof_byte, 1) : -1;

    close(control[1]);
    int holder_status = 0;
    pid_t holder_wait = holder_error == 0
        ? waitpid(holder, &holder_status, 0) : -1;
    close(output[0]);

    int ok = payload_length == 14 && strcmp(payload, "sibling-ready\n") == 0 &&
        producer_wait == producer && WIFEXITED(producer_status) &&
        WEXITSTATUS(producer_status) == 0 && poll_result > 0 && eof_read == 0 &&
        holder_wait == holder && WIFEXITED(holder_status) &&
        WEXITSTATUS(holder_status) == 0;
    if (!ok)
        fprintf(stderr,
            "spawn pipe writer lifetime: parent_closed_first=%d closefrom=%d producer_error=%d holder_error=%d payload_length=%zd producer_wait=%d producer_status=%d poll=%d revents=%#x eof_read=%zd holder_wait=%d holder_status=%d errno=%d\n",
            close_parent_writer_before_holder, holder_uses_closefrom,
            producer_error, holder_error, payload_length, (int)producer_wait,
            producer_status, poll_result, eof_poll.revents, eof_read,
            (int)holder_wait, holder_status, errno);
    printf("%s=%s\n",
        holder_uses_closefrom ? "spawn_closefrom_pipe_writer_eof" :
            "spawn_retained_writer_not_inherited",
        ok ? "yes" : "no");
    return ok;
}

static int test_spawn_closefrom_closes_unrelated_pipe_writer(const char *self) {
    return test_spawn_pipe_writer_lifetime(self, 0, 1);
}

static int test_spawn_retained_writer_is_not_inherited(const char *self) {
    return test_spawn_pipe_writer_lifetime(self, 1, 0);
}

static int test_waitpid_schedules_sibling(const char *self) {
    int transport[2] = {-1, -1};
    if (pipe(transport) != 0)
        return 0;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    int consumer_setup =
        posix_spawn_file_actions_adddup2(&actions, transport[0], STDIN_FILENO) == 0 &&
        posix_spawn_file_actions_addclose(&actions, transport[0]) == 0 &&
        posix_spawn_file_actions_addclose(&actions, transport[1]) == 0;
    char *consumer_argv[] = {(char *)self, "--waitpid-consumer", NULL};
    pid_t consumer = -1;
    int consumer_error = consumer_setup
        ? posix_spawnp(&consumer, self, &actions, NULL, consumer_argv, environ)
        : EINVAL;
    posix_spawn_file_actions_destroy(&actions);

    posix_spawn_file_actions_init(&actions);
    int producer_setup =
        posix_spawn_file_actions_adddup2(&actions, transport[1], STDOUT_FILENO) == 0 &&
        posix_spawn_file_actions_addclose(&actions, transport[0]) == 0 &&
        posix_spawn_file_actions_addclose(&actions, transport[1]) == 0;
    char *producer_argv[] = {(char *)self, "--waitpid-producer", NULL};
    pid_t producer = -1;
    int producer_error = consumer_error == 0 && producer_setup
        ? posix_spawnp(&producer, self, &actions, NULL, producer_argv, environ)
        : EINVAL;
    posix_spawn_file_actions_destroy(&actions);
    close(transport[0]);
    close(transport[1]);

    int consumer_status = 0;
    int producer_status = 0;
    pid_t consumer_wait = consumer_error == 0
        ? waitpid(consumer, &consumer_status, 0) : -1;
    pid_t producer_wait = producer_error == 0
        ? waitpid(producer, &producer_status, 0) : -1;
    int ok = consumer_wait == consumer && WIFEXITED(consumer_status) &&
        WEXITSTATUS(consumer_status) == 0 && producer_wait == producer &&
        WIFEXITED(producer_status) && WEXITSTATUS(producer_status) == 0;
    if (!ok)
        fprintf(stderr,
            "waitpid sibling scheduling: consumer_error=%d producer_error=%d consumer_wait=%d consumer_status=%d producer_wait=%d producer_status=%d errno=%d\n",
            consumer_error, producer_error, (int)consumer_wait, consumer_status,
            (int)producer_wait, producer_status, errno);
    printf("waitpid_schedules_sibling=%s\n", ok ? "yes" : "no");
    return ok;
}

static int child_stdout_closed(void) {
    struct stat stdout_stat;
    errno = 0;
    int fstat_closed = fstat(STDOUT_FILENO, &stdout_stat) == -1 &&
        errno == EBADF;
    errno = 0;
    int write_closed = write(STDOUT_FILENO, "x", 1) == -1 &&
        errno == EBADF;
    errno = 0;
    int close_closed = close(STDOUT_FILENO) == -1 && errno == EBADF;
    int ok = fstat_closed && write_closed && close_closed;
    if (!ok)
        fprintf(stderr,
            "spawn closed stdout child: fstat=%d write=%d close=%d errno=%d\n",
            fstat_closed, write_closed, close_closed, errno);
    return ok ? 0 : 86;
}

static int test_bidirectional_spawn_pipes(const char *self) {
    int requests[2];
    int responses[2];
    if (pipe(requests) != 0 || pipe(responses) != 0)
        return 0;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, requests[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, responses[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, requests[0]);
    posix_spawn_file_actions_addclose(&actions, requests[1]);
    posix_spawn_file_actions_addclose(&actions, responses[0]);
    posix_spawn_file_actions_addclose(&actions, responses[1]);
    char *child_argv[] = {(char *)self, "--bidirectional", NULL};
    pid_t child = -1;
    int spawn_error = posix_spawnp(&child, self, &actions, NULL,
        child_argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(requests[0]);
    close(responses[1]);
    if (spawn_error != 0) {
        close(requests[1]);
        close(responses[0]);
        return 0;
    }

    char response[32];
    int first = write(requests[1], "request-one\n", 12) == 12 &&
        read_line_fd(responses[0], response, sizeof(response)) == 13 &&
        strcmp(response, "response-one\n") == 0;
    ssize_t second_write = first
        ? write(requests[1], "request-two\n", 12) : -2;
    int second_write_errno = errno;
    ssize_t second_read = second_write == 12
        ? read_line_fd(responses[0], response, sizeof(response)) : -2;
    int second_read_errno = errno;
    int second = first && second_write == 12 && second_read == 13 &&
        strcmp(response, "response-two\n") == 0;
    errno = 0;
    int request_close = close(requests[1]);
    int request_close_errno = errno;
    close(responses[0]);
    int status = 0;
    errno = 0;
    pid_t wait_result = waitpid(child, &status, 0);
    int wait_errno = errno;
    int child_ok = wait_result == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0;
    int ok = first && second && child_ok;
    if (!ok)
        fprintf(stderr,
            "spawn bidirectional: request_fds=%d,%d response_fds=%d,%d first=%d second=%d second_write=%zd write_errno=%d second_read=%zd read_errno=%d close=%d close_errno=%d wait=%d wait_errno=%d status=%d child_ok=%d response=%s\n",
            requests[0], requests[1], responses[0], responses[1],
            first, second, second_write, second_write_errno, second_read,
            second_read_errno, request_close, request_close_errno,
            (int)wait_result, wait_errno, status, child_ok, response);
    printf("spawn_bidirectional_pipes=%s\n", ok ? "yes" : "no");
    return ok;
}

static int test_spawn_stdio_lifecycle(const char *self) {
    int saved_stdout = dup(STDOUT_FILENO);
    int output_pipe[2] = {-1, -1};
    if (saved_stdout < 0 || pipe(output_pipe) != 0) {
        if (saved_stdout >= 0)
            close(saved_stdout);
        return 0;
    }

    int redirected = dup2(output_pipe[1], STDOUT_FILENO) == STDOUT_FILENO;
    if (close(output_pipe[1]) != 0)
        redirected = 0;
    output_pipe[1] = -1;

    char *noop_argv[] = {(char *)self, "--noop", NULL};
    int inherited_status = 0;
    int inherited_error = redirected
        ? spawn_self(self, NULL, NULL, noop_argv, &inherited_status)
        : EINVAL;
    int inherited_child_ok = inherited_error == 0 &&
        WIFEXITED(inherited_status) && WEXITSTATUS(inherited_status) == 0;

    char *closed_argv[] = {(char *)self, "--stdout-closed", NULL};
    posix_spawn_file_actions_t actions;
    int actions_initialized = posix_spawn_file_actions_init(&actions) == 0;
    int actions_ready = actions_initialized &&
        posix_spawn_file_actions_addclose(&actions, STDOUT_FILENO) == 0;
    int closed_status = 0;
    int closed_error = actions_ready
        ? spawn_self(self, &actions, NULL, closed_argv, &closed_status)
        : EINVAL;
    if (actions_initialized)
        posix_spawn_file_actions_destroy(&actions);
    int closed_child_ok = closed_error == 0 && WIFEXITED(closed_status) &&
        WEXITSTATUS(closed_status) == 0;

    struct stat stdout_stat;
    int parent_open_after_spawn = fstat(STDOUT_FILENO, &stdout_stat) == 0;
    int parent_write_after_spawn =
        write(STDOUT_FILENO, "parent-after-spawn", 18) == 18;
    errno = 0;
    int first_close = close(STDOUT_FILENO) == 0;
    errno = 0;
    int fstat_closed = fstat(STDOUT_FILENO, &stdout_stat) == -1 &&
        errno == EBADF;
    errno = 0;
    int second_close = close(STDOUT_FILENO) == -1 && errno == EBADF;
    int restored = dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO;
    close(saved_stdout);
    int restored_fstat = fstat(STDOUT_FILENO, &stdout_stat) == 0;
    close(output_pipe[0]);

    int spawn_borrows = inherited_child_ok && closed_child_ok &&
        parent_open_after_spawn && parent_write_after_spawn;
    int close_restore = first_close && fstat_closed && second_close &&
        restored && restored_fstat;
    if (!spawn_borrows || !close_restore)
        fprintf(stderr,
            "spawn stdio lifecycle: redirected=%d inherited_error=%d inherited_status=%d closed_error=%d closed_status=%d parent_open=%d parent_write=%d first_close=%d fstat_closed=%d second_close=%d restored=%d restored_fstat=%d\n",
            redirected, inherited_error, inherited_status, closed_error,
            closed_status, parent_open_after_spawn, parent_write_after_spawn,
            first_close, fstat_closed, second_close, restored, restored_fstat);
    printf("spawn_borrows_parent_stdout=%s\n", spawn_borrows ? "yes" : "no");
    printf("spawn_closed_stdout_ebadf=%s\n", closed_child_ok ? "yes" : "no");
    printf("stdio_close_restore=%s\n", close_restore ? "yes" : "no");
    return redirected && spawn_borrows && close_restore;
}

static int test_actions(const char *self) {
    char directory[] = "/tmp/agentos-spawn-actions-XXXXXX";
    if (mkdtemp(directory) == NULL)
        return 0;
    char path[600];
    if (snprintf(path, sizeof(path), "%s/payload", directory) >= (int)sizeof(path))
        return 0;
    int writer = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (writer < 0 || write(writer, "action", 6) != 6 || close(writer) != 0)
        return 0;
    int directory_fd = open(directory, O_RDONLY | O_DIRECTORY);
    if (directory_fd < 0)
        return 0;

    const int opened_fd = 40;
    const int duplicated_fd = 41;
    const int inherited_fd = 43;
    if (dup2(directory_fd, inherited_fd) != inherited_fd) {
        close(directory_fd);
        return 0;
    }
    char directory_fd_arg[16];
    char opened_fd_arg[16];
    char duplicated_fd_arg[16];
    char inherited_fd_arg[16];
    snprintf(directory_fd_arg, sizeof(directory_fd_arg), "%d", directory_fd);
    snprintf(opened_fd_arg, sizeof(opened_fd_arg), "%d", opened_fd);
    snprintf(duplicated_fd_arg, sizeof(duplicated_fd_arg), "%d", duplicated_fd);
    snprintf(inherited_fd_arg, sizeof(inherited_fd_arg), "%d", inherited_fd);
    char *child_argv[] = {
        (char *)self, "--actions", directory, directory_fd_arg,
        opened_fd_arg, duplicated_fd_arg, inherited_fd_arg, NULL,
    };
    posix_spawn_file_actions_t actions;
    int status = 0;
    int setup = posix_spawn_file_actions_init(&actions) == 0 &&
        posix_spawn_file_actions_addfchdir_np(&actions, directory_fd) == 0 &&
        posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1) == 0 &&
        posix_spawn_file_actions_addopen(&actions, opened_fd, "payload",
            O_RDONLY | O_CLOEXEC, 0) == 0 &&
        posix_spawn_file_actions_adddup2(&actions, opened_fd, duplicated_fd) == 0 &&
        posix_spawn_file_actions_addclose(&actions, opened_fd) == 0;
    int spawn_error = setup ? spawn_self(self, &actions, NULL, child_argv, &status) : EINVAL;
    posix_spawn_file_actions_destroy(&actions);
    int child_ok = spawn_error == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    int parent_unchanged = fcntl(directory_fd, F_GETFD) >= 0 &&
        fcntl(inherited_fd, F_GETFD) >= 0;

    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, directory_fd);
    posix_spawn_file_actions_adddup2(&actions, directory_fd, 42);
    pid_t failed_child = -1;
    char *noop_argv[] = {(char *)self, "--pgroup", NULL};
    int failure = posix_spawnp(&failed_child, self, &actions, NULL,
        noop_argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    errno = 0;
    int no_child = waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD;
    int failure_atomic = failure == EBADF && failed_child == -1 &&
        fcntl(directory_fd, F_GETFD) >= 0 && no_child;

    close(directory_fd);
    close(inherited_fd);
    unlink(path);
    rmdir(directory);
    printf("spawn_actions_ordered=%s\n", child_ok ? "yes" : "no");
    printf("spawn_actions_parent_unchanged=%s\n",
        parent_unchanged && failure_atomic ? "yes" : "no");
    printf("spawn_closefrom_ordered=%s\n", child_ok ? "yes" : "no");
    return child_ok && parent_unchanged && failure_atomic;
}

static int test_inheritance(const char *self) {
    char path[] = "/tmp/agentos-spawn-inherit-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0 || write(fd, "abcdef", 6) != 6 || lseek(fd, 1, SEEK_SET) != 1)
        return 0;
    const int shared_fd = 50;
    const int cloexec_fd = 51;
    if (dup2(fd, shared_fd) != shared_fd || dup2(fd, cloexec_fd) != cloexec_fd ||
        fcntl(cloexec_fd, F_SETFD, FD_CLOEXEC) != 0) {
        close(fd);
        unlink(path);
        return 0;
    }
    close(fd);
    char shared_arg[16];
    char cloexec_arg[16];
    snprintf(shared_arg, sizeof(shared_arg), "%d", shared_fd);
    snprintf(cloexec_arg, sizeof(cloexec_arg), "%d", cloexec_fd);
    char *child_argv[] = {
        (char *)self, "--inherit", shared_arg, cloexec_arg, NULL,
    };
    int status = 0;
    int error = spawn_self(self, NULL, NULL, child_argv, &status);
    char next = 0;
    int shared_offset = read(shared_fd, &next, 1) == 1 && next == 'c';
    int parent_cloexec_open = fcntl(cloexec_fd, F_GETFD) >= 0;
    close(shared_fd);
    close(cloexec_fd);
    unlink(path);
    int ok = error == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        shared_offset && parent_cloexec_open;
    printf("spawn_inherit_shared_description=%s\n", ok ? "yes" : "no");
    return ok;
}

static int test_same_fd_dup2_clears_cloexec(const char *self) {
    char path[] = "/tmp/agentos-spawn-same-fd-XXXXXX";
    int source = mkstemp(path);
    const int target = 52;
    if (source < 0 || dup2(source, target) != target ||
        fcntl(target, F_SETFD, FD_CLOEXEC) != 0) {
        if (source >= 0)
            close(source);
        unlink(path);
        return 0;
    }
    close(source);

    char target_arg[16];
    snprintf(target_arg, sizeof(target_arg), "%d", target);
    char *child_argv[] = {
        (char *)self, "--same-fd-dup2", target_arg, NULL,
    };
    posix_spawn_file_actions_t actions;
    int initialized = posix_spawn_file_actions_init(&actions) == 0;
    int setup = initialized &&
        posix_spawn_file_actions_adddup2(&actions, target, target) == 0;
    int status = 0;
    int error = setup
        ? spawn_self(self, &actions, NULL, child_argv, &status) : EINVAL;
    if (initialized)
        posix_spawn_file_actions_destroy(&actions);
    int parent_still_cloexec =
        (fcntl(target, F_GETFD) & FD_CLOEXEC) != 0;
    close(target);
    unlink(path);
    int ok = error == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        parent_still_cloexec;
    printf("spawn_same_fd_dup2_clears_cloexec=%s\n", ok ? "yes" : "no");
    return ok;
}

static int test_live_cwd_inherited_with_file_actions(const char *self) {
    char directory[] = "/tmp/agentos-spawn-live-cwd-XXXXXX";
    char original_cwd[512];
    if (getcwd(original_cwd, sizeof(original_cwd)) == NULL)
        return 0;
    int original_cwd_fd = open(".", O_RDONLY | O_DIRECTORY);
    if (original_cwd_fd < 0 || mkdtemp(directory) == NULL) {
        if (original_cwd_fd >= 0)
            close(original_cwd_fd);
        return 0;
    }

    posix_spawn_file_actions_t actions;
    int status = 0;
    int actions_initialized = 0;
    int setup = chdir(directory) == 0;
    if (setup && posix_spawn_file_actions_init(&actions) == 0) {
        actions_initialized = 1;
        setup = posix_spawn_file_actions_addclose(&actions, original_cwd_fd) == 0;
    } else {
        setup = 0;
    }
    char *child_argv[] = {(char *)self, "--live-cwd", directory, NULL};
    int spawn_error = setup
        ? spawn_self(self, &actions, NULL, child_argv, &status)
        : EINVAL;
    if (actions_initialized)
        posix_spawn_file_actions_destroy(&actions);
    int restored = chdir(original_cwd) == 0;
    close(original_cwd_fd);
    (void)rmdir(directory);

    int ok = spawn_error == 0 && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0 && restored;
    printf("spawn_live_cwd_inherited=%s\n", ok ? "yes" : "no");
    return ok;
}

static int write_spawn_path_fixture(const char *path, const char *payload) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    size_t length = strlen(payload);
    int ok = fd >= 0 && write(fd, payload, length) == (ssize_t)length;
    if (fd >= 0 && close(fd) != 0)
        ok = 0;
    return ok;
}

static int spawn_path_fixture_matches(const char *path, const char *payload) {
    char buffer[64] = {0};
    int fd = open(path, O_RDONLY);
    size_t length = strlen(payload);
    ssize_t read_length = fd >= 0 ? read(fd, buffer, sizeof(buffer)) : -1;
    if (fd >= 0)
        close(fd);
    return read_length == (ssize_t)length && memcmp(buffer, payload, length) == 0;
}

static int spawn_path_fixture_empty(const char *path) {
    struct stat stat_buffer;
    return stat(path, &stat_buffer) == 0 && stat_buffer.st_size == 0;
}

static int test_spawn_action_paths_follow_canonical_cwd(const char *self) {
    char root[] = "/tmp/agentos-spawn-canonical-cwd-XXXXXX";
    char real[640];
    char chdir_parent[640];
    char chdir_directory[640];
    char chdir_target[640];
    char chdir_alias[640];
    char fchdir_parent[640];
    char fchdir_directory[640];
    char fchdir_target[640];
    char fchdir_alias[640];
    char non_target[640];
    if (mkdtemp(root) == NULL ||
        snprintf(real, sizeof(real), "%s/real", root) >= (int)sizeof(real) ||
        snprintf(chdir_parent, sizeof(chdir_parent), "%s/chdir", real) >=
            (int)sizeof(chdir_parent) ||
        snprintf(chdir_directory, sizeof(chdir_directory), "%s/subdirectory",
            chdir_parent) >= (int)sizeof(chdir_directory) ||
        snprintf(chdir_target, sizeof(chdir_target), "%s/target", chdir_parent) >=
            (int)sizeof(chdir_target) ||
        snprintf(chdir_alias, sizeof(chdir_alias), "%s/chdir-alias", root) >=
            (int)sizeof(chdir_alias) ||
        snprintf(fchdir_parent, sizeof(fchdir_parent), "%s/fchdir", real) >=
            (int)sizeof(fchdir_parent) ||
        snprintf(fchdir_directory, sizeof(fchdir_directory), "%s/subdirectory",
            fchdir_parent) >= (int)sizeof(fchdir_directory) ||
        snprintf(fchdir_target, sizeof(fchdir_target), "%s/target", fchdir_parent) >=
            (int)sizeof(fchdir_target) ||
        snprintf(fchdir_alias, sizeof(fchdir_alias), "%s/fchdir-alias", root) >=
            (int)sizeof(fchdir_alias) ||
        snprintf(non_target, sizeof(non_target), "%s/target", root) >=
            (int)sizeof(non_target))
        return 0;
    if (mkdir(real, 0700) != 0 || mkdir(chdir_parent, 0700) != 0 ||
        mkdir(chdir_directory, 0700) != 0 || mkdir(fchdir_parent, 0700) != 0 ||
        mkdir(fchdir_directory, 0700) != 0 ||
        !write_spawn_path_fixture(chdir_target, "canonical-chdir-target") ||
        !write_spawn_path_fixture(fchdir_target, "canonical-fchdir-target") ||
        !write_spawn_path_fixture(non_target, "must-survive") ||
        symlink(chdir_directory, chdir_alias) != 0 ||
        symlink(fchdir_directory, fchdir_alias) != 0)
        return 0;

    char *noop_argv[] = {(char *)self, "--noop", NULL};
    posix_spawn_file_actions_t actions;
    int status = 0;
    int initialized = posix_spawn_file_actions_init(&actions) == 0;
    int setup = initialized &&
        posix_spawn_file_actions_addchdir_np(&actions, chdir_alias) == 0 &&
        posix_spawn_file_actions_addopen(&actions, 70, "../target",
            O_WRONLY | O_TRUNC, 0) == 0;
    int spawn_error = setup
        ? spawn_self(self, &actions, NULL, noop_argv, &status) : EINVAL;
    if (initialized)
        posix_spawn_file_actions_destroy(&actions);
    int chdir_ok = spawn_error == 0 && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0 && spawn_path_fixture_empty(chdir_target) &&
        spawn_path_fixture_matches(non_target, "must-survive");

    status = 0;
    initialized = posix_spawn_file_actions_init(&actions) == 0;
    setup = initialized &&
        posix_spawn_file_actions_addopen(&actions, 71, fchdir_alias,
            O_RDONLY | O_DIRECTORY, 0) == 0 &&
        posix_spawn_file_actions_addfchdir_np(&actions, 71) == 0 &&
        posix_spawn_file_actions_addopen(&actions, 72, "../target",
            O_WRONLY | O_TRUNC, 0) == 0;
    spawn_error = setup
        ? spawn_self(self, &actions, NULL, noop_argv, &status) : EINVAL;
    if (initialized)
        posix_spawn_file_actions_destroy(&actions);
    int fchdir_ok = spawn_error == 0 && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0 && spawn_path_fixture_empty(fchdir_target) &&
        spawn_path_fixture_matches(non_target, "must-survive");

    unlink(chdir_alias);
    unlink(fchdir_alias);
    unlink(chdir_target);
    unlink(fchdir_target);
    unlink(non_target);
    rmdir(chdir_directory);
    rmdir(chdir_parent);
    rmdir(fchdir_directory);
    rmdir(fchdir_parent);
    rmdir(real);
    rmdir(root);
    printf("spawn_chdir_symlink_relative_open=%s\n", chdir_ok ? "yes" : "no");
    printf("spawn_fchdir_symlink_relative_open=%s\n", fchdir_ok ? "yes" : "no");
    return chdir_ok && fchdir_ok;
}

static int test_spawnp_actions_once(const char *self) {
    char first_directory[] = "/tmp/agentos-spawnp-first-XXXXXX";
    char action_target[] = "/tmp/agentos-spawnp-action-XXXXXX";
    int placeholder = mkstemp(action_target);
    if (placeholder < 0 || close(placeholder) != 0 || unlink(action_target) != 0 ||
        mkdtemp(first_directory) == NULL)
        return 0;

    const char *name = strrchr(self, '/');
    name = name == NULL ? self : name + 1;
    char denied_candidate[700];
    if (snprintf(denied_candidate, sizeof(denied_candidate), "%s/%s",
            first_directory, name) >= (int)sizeof(denied_candidate))
        return 0;
    int denied = open(denied_candidate, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (denied < 0 || write(denied, "not executable\n", 15) != 15 ||
        close(denied) != 0)
        return 0;

    const char *old_path_value = getenv("PATH");
    char *old_path = old_path_value == NULL ? NULL : strdup(old_path_value);
    const char *slash = strrchr(self, '/');
    size_t executable_dir_len = slash == NULL ? 0 : (size_t)(slash - self);
    const char *fallback = slash == NULL
        ? (old_path != NULL ? old_path : ".") : self;
    size_t fallback_len = slash == NULL ? strlen(fallback) : executable_dir_len;
    size_t search_len = strlen(first_directory) + 1 + fallback_len;
    char *search = malloc(search_len + 1);
    if (search == NULL) {
        free(old_path);
        return 0;
    }
    snprintf(search, search_len + 1, "%s:%.*s", first_directory,
        (int)fallback_len, fallback);
    int path_set = setenv("PATH", search, 1) == 0;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    int setup = posix_spawn_file_actions_addopen(&actions, 60, action_target,
        O_WRONLY | O_CREAT | O_EXCL, 0600) == 0 &&
        posix_spawn_file_actions_addclose(&actions, 60) == 0;
    pid_t child = -1;
    char *child_argv[] = {(char *)self, "--noop", NULL};
    int spawn_error = path_set && setup
        ? posix_spawnp(&child, name, &actions, NULL, child_argv, environ)
        : EINVAL;
    posix_spawn_file_actions_destroy(&actions);
    int child_ok = spawn_error == 0 && wait_exit(child, 0);
    int action_once = access(action_target, F_OK) == 0;

    if (old_path != NULL)
        setenv("PATH", old_path, 1);
    else
        unsetenv("PATH");
    free(old_path);
    free(search);
    unlink(action_target);
    unlink(denied_candidate);
    rmdir(first_directory);
    int ok = child_ok && action_once;
    if (!ok)
        fprintf(stderr,
            "spawnp actions once: spawn_error=%d child=%d child_ok=%d action_once=%d\n",
            spawn_error, (int)child, child_ok, action_once);
    printf("spawnp_path_actions_once=%s\n", ok ? "yes" : "no");
    return ok;
}

static char *spawnp_executable_directory(const char *self) {
    const char *slash = strrchr(self, '/');
    if (slash != NULL)
        return slash == self ? strdup("/") : strndup(self, (size_t)(slash - self));

    const char *path = getenv("PATH");
    if (path == NULL)
        path = "/bin:/usr/bin";
    const char *entry = path;
    for (;;) {
        const char *separator = strchr(entry, ':');
        size_t entry_length = separator == NULL
            ? strlen(entry) : (size_t)(separator - entry);
        const char *directory = entry_length == 0 ? "." : entry;
        size_t directory_length = entry_length == 0 ? 1 : entry_length;
        size_t candidate_length = directory_length + 1 + strlen(self) + 1;
        char *candidate = malloc(candidate_length);
        if (candidate == NULL)
            return NULL;
        snprintf(candidate, candidate_length, "%.*s/%s",
            (int)directory_length, directory, self);
        int executable = access(candidate, X_OK) == 0;
        free(candidate);
        if (executable)
            return strndup(directory, directory_length);
        if (separator == NULL)
            return NULL;
        entry = separator + 1;
    }
}

static int test_spawnp_empty_path_uses_current_directory(const char *self) {
    char *directory = spawnp_executable_directory(self);
    char *old_cwd = getcwd(NULL, 0);
    const char *old_path_value = getenv("PATH");
    char *old_path = old_path_value == NULL ? NULL : strdup(old_path_value);
    const char *name = strrchr(self, '/');
    name = name == NULL ? self : name + 1;
    int setup = directory != NULL && old_cwd != NULL &&
        (old_path_value == NULL || old_path != NULL) &&
        chdir(directory) == 0 && setenv("PATH", "", 1) == 0;
    pid_t child = -1;
    char *child_argv[] = {(char *)name, "--noop", NULL};
    int spawn_error = setup
        ? posix_spawnp(&child, name, NULL, NULL, child_argv, environ)
        : EINVAL;

    if (old_path != NULL)
        setenv("PATH", old_path, 1);
    else
        unsetenv("PATH");
    int restored = old_cwd != NULL && chdir(old_cwd) == 0;
    int child_ok = spawn_error == 0 && wait_exit(child, 0);
    free(directory);
    free(old_cwd);
    free(old_path);

    int ok = setup && restored && child_ok;
    if (!ok)
        fprintf(stderr,
            "spawnp empty PATH: setup=%d restored=%d spawn_error=%d child=%d child_ok=%d\n",
            setup, restored, spawn_error, (int)child, child_ok);
    printf("spawnp_empty_path_current_directory=%s\n", ok ? "yes" : "no");
    return ok;
}

static int test_parent_signal_mask(void) {
    struct sigaction action;
    sigset_t set;
    sigset_t usr1;
    memset(&action, 0, sizeof(action));
    action.sa_handler = usr1_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0 || sigemptyset(&set) != 0 ||
        sigaddset(&set, SIGUSR1) != 0 || sigaddset(&set, SIGKILL) != 0 ||
        sigaddset(&set, SIGSTOP) != 0 || sigprocmask(SIG_BLOCK, &set, NULL) != 0)
        return 0;
    int usr1_blocked = query_mask_member(SIGUSR1);
    int kill_blocked = query_mask_member(SIGKILL);
    int stop_blocked = query_mask_member(SIGSTOP);
    int unmaskable = usr1_blocked == 1 && kill_blocked == 0 && stop_blocked == 0;
    if (!unmaskable)
        fprintf(stderr, "signal mask members: USR1=%d KILL=%d STOP=%d errno=%d\n",
            usr1_blocked, kill_blocked, stop_blocked, errno);
    if (kill(getpid(), SIGUSR1) != 0 || kill(getpid(), SIGUSR1) != 0)
        return 0;
    usleep(1000);
    int delayed = usr1_calls == 0;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    if (sigprocmask(SIG_UNBLOCK, &usr1, NULL) != 0)
        return 0;
    usleep(1000);
    int coalesced = usr1_calls == 1;
    printf("signal_mask_unmaskable=%s\n", unmaskable ? "yes" : "no");
    printf("signal_mask_pending_coalesced=%s\n",
        delayed && coalesced ? "yes" : "no");
    return unmaskable && delayed && coalesced;
}

static int test_spawn_masks(const char *self) {
    sigset_t usr1;
    sigset_t empty;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    sigemptyset(&empty);
    if (sigprocmask(SIG_BLOCK, &usr1, NULL) != 0)
        return 0;

    char *blocked_argv[] = {(char *)self, "--mask-query", "blocked", NULL};
    int status = 0;
    int inherited_error = spawn_self(self, NULL, NULL, blocked_argv, &status);
    int inherited = inherited_error == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setsigmask(&attributes, &empty);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK);
    char *clear_argv[] = {(char *)self, "--mask-query", "clear", NULL};
    int clear_error = spawn_self(self, NULL, &attributes, clear_argv, &status);
    int cleared = clear_error == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;

    posix_spawnattr_setsigmask(&attributes, &usr1);
    char *delivery_argv[] = {(char *)self, "--mask-delivery", NULL};
    int delivery_error = spawn_self(self, NULL, &attributes, delivery_argv, &status);
    int delivery = delivery_error == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    posix_spawnattr_destroy(&attributes);
    sigprocmask(SIG_UNBLOCK, &usr1, NULL);
    int ok = inherited && cleared && delivery;
    printf("spawn_signal_masks=%s\n", ok ? "yes" : "no");
    return ok;
}

static int test_spawn_defaults(const char *self) {
    struct sigaction ignored;
    struct sigaction original;
    memset(&ignored, 0, sizeof(ignored));
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    if (sigaction(SIGPIPE, &ignored, &original) != 0)
        return 0;
    char *child_argv[] = {(char *)self, "--sigpipe", NULL};
    int status = 0;
    int ignored_error = spawn_self(self, NULL, NULL, child_argv, &status);
    int ignored_status = status;
    int inherited_ignore = ignored_error == 0 && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0;

    posix_spawnattr_t attributes;
    sigset_t defaults;
    posix_spawnattr_init(&attributes);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF);
    int default_error = spawn_self(self, NULL, &attributes, child_argv, &status);
    int reset_default = default_error == 0 && WIFSIGNALED(status) &&
        WTERMSIG(status) == SIGPIPE;
    posix_spawnattr_destroy(&attributes);
    sigaction(SIGPIPE, &original, NULL);
    int ok = inherited_ignore && reset_default;
    if (!ok)
        fprintf(stderr,
            "spawn defaults: ignored_error=%d ignored_status=%d inherited=%d default_error=%d default_status=%d reset=%d\n",
            ignored_error, ignored_status, inherited_ignore, default_error, status,
            reset_default);
    printf("spawn_signal_defaults=%s\n", ok ? "yes" : "no");
    return ok;
}

static int test_signal_handler_semantics(const char *self) {
    struct sigaction usr1_action;
    struct sigaction usr2_action;
    memset(&usr1_action, 0, sizeof(usr1_action));
    memset(&usr2_action, 0, sizeof(usr2_action));
    usr1_action.sa_handler = masked_usr1_handler;
    usr2_action.sa_handler = usr2_handler;
    sigemptyset(&usr1_action.sa_mask);
    sigaddset(&usr1_action.sa_mask, SIGUSR2);
    sigemptyset(&usr2_action.sa_mask);
    if (sigaction(SIGUSR1, &usr1_action, NULL) != 0 ||
        sigaction(SIGUSR2, &usr2_action, NULL) != 0 ||
        kill(getpid(), SIGUSR1) != 0)
        return 0;
    usleep(1000);
    int handler_mask_ok = masked_usr1_calls == 2 &&
        masked_usr1_max_depth == 1 && usr2_calls == 1 &&
        usr2_during_usr1 == 0;

    char *child_argv[] = {(char *)self, "--resethand", NULL};
    int status = 0;
    int reset_error = spawn_self(self, NULL, NULL, child_argv, &status);
    int reset_ok = reset_error == 0 && WIFSIGNALED(status) &&
        WTERMSIG(status) == SIGUSR1;
    int ok = handler_mask_ok && reset_ok;
    if (!ok)
        fprintf(stderr,
            "signal semantics: handler_ok=%d usr1_calls=%d max_depth=%d usr2_calls=%d during_usr1=%d reset_error=%d reset_status=%d reset_ok=%d\n",
            handler_mask_ok, (int)masked_usr1_calls,
            (int)masked_usr1_max_depth, (int)usr2_calls,
            (int)usr2_during_usr1, reset_error, status, reset_ok);
    printf("signal_handler_mask_and_reset=%s\n", ok ? "yes" : "no");
    return ok;
}

static int test_spawn_attributes(const char *self) {
    posix_spawnattr_t attributes;
    int status = 0;
    char *child_argv[] = {(char *)self, "--pgroup", NULL};
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setpgroup(&attributes, 0);
    int flag_error = posix_spawnattr_setflags(&attributes,
        POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_USEVFORK);
    int group_error = flag_error == 0
        ? spawn_self(self, NULL, &attributes, child_argv, &status) : flag_error;
    int group_ok = group_error == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;

    pid_t parent_group = getpgrp();
    char parent_group_arg[16];
    snprintf(parent_group_arg, sizeof(parent_group_arg), "%d", (int)parent_group);
    char *joined_argv[] = {(char *)self, "--pgroup", parent_group_arg, NULL};
    posix_spawnattr_setpgroup(&attributes, parent_group);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    int joined_error = spawn_self(self, NULL, &attributes, joined_argv, &status);
    int joined_ok = joined_error == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;

    errno = 0;
    int invalid_get = getpgid(-1) == -1 && errno == ESRCH;
    errno = 0;
    int missing_get = getpgid(2147483647) == -1 && errno == ESRCH;
    errno = 0;
    int invalid_pid = setpgid(-1, 0) == -1 && errno == EINVAL;
    errno = 0;
    int invalid_group = setpgid(0, -1) == -1 && errno == EINVAL;
    int group_errors_ok = invalid_get && missing_get && invalid_pid && invalid_group;
    if (!group_ok || !joined_ok || !group_errors_ok)
        fprintf(stderr,
            "spawn groups: new_error=%d new_status=%d joined_error=%d joined_status=%d invalid_get=%d missing_get=%d invalid_pid=%d invalid_group=%d\n",
            group_error, status, joined_error, status, invalid_get, missing_get,
            invalid_pid, invalid_group);

    pid_t child = -1;
    char *noop_argv[] = {(char *)self, "--noop", NULL};
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_RESETIDS);
    int resetids_error = posix_spawnp(&child, self, NULL, &attributes,
        noop_argv, environ);
    int resetids_ok = resetids_error == 0 && wait_exit(child, 0);

    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    child = -1;
    int setsid_error = posix_spawnp(&child, self, NULL, &attributes,
        child_argv, environ);
    int setsid_ok = setsid_error == 0 && wait_exit(child, 0);

    struct sched_param parameter;
    memset(&parameter, 0, sizeof(parameter));
    posix_spawnattr_setschedparam(&attributes, &parameter);
    posix_spawnattr_setschedpolicy(&attributes, SCHED_OTHER);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSCHEDPARAM);
    child = -1;
    int schedparam_error = posix_spawnp(&child, self, NULL, &attributes,
        noop_argv, environ);
    int schedparam_ok = schedparam_error == 0 && wait_exit(child, 0);

    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSCHEDULER);
    child = -1;
    int scheduler_error = posix_spawnp(&child, self, NULL, &attributes,
        noop_argv, environ);
    int scheduler_ok = scheduler_error == 0 && wait_exit(child, 0);

    parameter.sched_priority = 1;
    posix_spawnattr_setschedparam(&attributes, &parameter);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSCHEDPARAM);
    child = -1;
    int invalid_scheduler = posix_spawnp(&child, self, NULL, &attributes,
        noop_argv, environ);
    posix_spawnattr_destroy(&attributes);
    errno = 0;
    int no_child = waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD;
    int invalid_scheduler_ok = invalid_scheduler == EINVAL && child == -1 && no_child;
    int attrs_ok = setsid_ok && schedparam_ok && scheduler_ok &&
        invalid_scheduler_ok;
    if (!attrs_ok)
        fprintf(stderr,
            "spawn attrs: setsid=%d schedparam=%d scheduler=%d invalid=%d child=%d no_child=%d\n",
            setsid_error, schedparam_error, scheduler_error,
            invalid_scheduler, (int)child, no_child);
    printf("spawn_pgroup=%s\n",
        group_ok && joined_ok && group_errors_ok ? "yes" : "no");
    printf("spawn_resetids=%s\n", resetids_ok ? "yes" : "no");
    printf("spawn_setsid=%s\n", setsid_ok ? "yes" : "no");
    printf("spawn_scheduler_attrs=%s\n",
        schedparam_ok && scheduler_ok && invalid_scheduler_ok ? "yes" : "no");
    return group_ok && joined_ok && group_errors_ok && resetids_ok && attrs_ok;
}

int main(int argc, char **argv) {
    /* Child and parent processes share the captured stdout description. Make
     * their ordering explicit instead of depending on glibc vs musl's first-
     * write buffering decision for a non-TTY stdout. */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc >= 2 && strcmp(argv[1], "--actions") == 0)
        return child_actions(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--inherit") == 0)
        return child_inherit(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--same-fd-dup2") == 0)
        return child_same_fd_dup2(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--live-cwd") == 0)
        return child_live_cwd(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "--mask-query") == 0)
        return child_mask_query(argc, argv);
    if (argc == 2 && strcmp(argv[1], "--mask-delivery") == 0)
        return child_mask_delivery();
    if (argc == 2 && strcmp(argv[1], "--sigpipe") == 0)
        return child_sigpipe();
    if (argc == 2 && strcmp(argv[1], "--resethand") == 0)
        return child_resethand();
    if (argc >= 2 && strcmp(argv[1], "--pgroup") == 0)
        return child_pgroup(argc, argv);
    if (argc == 2 && strcmp(argv[1], "--bidirectional-parent") == 0)
        return test_bidirectional_spawn_pipes(argv[0]) ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--bidirectional") == 0)
        return child_bidirectional();
    if (argc == 2 && strcmp(argv[1], "--waitpid-sibling-parent") == 0)
        return test_waitpid_schedules_sibling(argv[0]) ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--waitpid-producer") == 0)
        return child_waitpid_producer();
    if (argc == 2 && strcmp(argv[1], "--waitpid-consumer") == 0)
        return child_waitpid_consumer();
    if (argc == 2 && strcmp(argv[1], "--hold-stdin") == 0)
        return child_hold_stdin_until_eof();
    if (argc == 2 && strcmp(argv[1], "--closefrom-pipe-parent") == 0)
        return test_spawn_closefrom_closes_unrelated_pipe_writer(argv[0]) ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--retained-pipe-parent") == 0)
        return test_spawn_retained_writer_is_not_inherited(argv[0]) ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--stdout-closed") == 0)
        return child_stdout_closed();
    if (argc == 2 && strcmp(argv[1], "--noop") == 0)
        return 0;

    int ok = 1;
    ok &= test_actions(argv[0]);
    ok &= test_inheritance(argv[0]);
    ok &= test_same_fd_dup2_clears_cloexec(argv[0]);
    ok &= test_live_cwd_inherited_with_file_actions(argv[0]);
    ok &= test_spawn_action_paths_follow_canonical_cwd(argv[0]);
    ok &= test_spawnp_actions_once(argv[0]);
    ok &= test_spawnp_empty_path_uses_current_directory(argv[0]);
    ok &= test_parent_signal_mask();
    ok &= test_spawn_masks(argv[0]);
    ok &= test_spawn_defaults(argv[0]);
    ok &= test_signal_handler_semantics(argv[0]);
    ok &= test_spawn_attributes(argv[0]);
    ok &= test_bidirectional_spawn_pipes(argv[0]);
    ok &= test_waitpid_schedules_sibling(argv[0]);
    ok &= test_spawn_closefrom_closes_unrelated_pipe_writer(argv[0]);
    ok &= test_spawn_retained_writer_is_not_inherited(argv[0]);
    ok &= test_spawn_stdio_lifecycle(argv[0]);
    printf("spawn_contract=%s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}
