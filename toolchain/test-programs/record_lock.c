#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static const char *path = "/tmp/agentos-record-lock-contract";
static volatile sig_atomic_t alarm_seen;

static void on_alarm(int signal) {
    (void)signal;
    alarm_seen = 1;
}

static int open_contract_file(void) {
    return open(path, O_CREAT | O_RDWR, 0600);
}

static int set_lock(int fd, int command, short type, off_t start, off_t length) {
    struct flock lock = {
        .l_type = type,
        .l_whence = SEEK_SET,
        .l_start = start,
        .l_len = length,
    };
    return fcntl(fd, command, &lock);
}

static int spawn_mode(const char *self, const char *mode) {
    pid_t pid;
    char *const argv[] = {(char *)self, (char *)mode, NULL};
    int error = posix_spawnp(&pid, self, NULL, NULL, argv, environ);
    if (error != 0) return 100 + error;
    int status;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) return 200;
    return WEXITSTATUS(status);
}

static int spawn_captured_mode(const char *self, const char *mode,
                               char expected, pid_t *ret_pid) {
    int ready[2];
    if (pipe(ready) != 0) return 50;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, ready[1], STDOUT_FILENO) != 0 ||
        posix_spawn_file_actions_addclose(&actions, ready[0]) != 0 ||
        posix_spawn_file_actions_addclose(&actions, ready[1]) != 0) {
        close(ready[0]);
        close(ready[1]);
        return 51;
    }
    char *const argv[] = {(char *)self, (char *)mode, NULL};
    int error = posix_spawnp(ret_pid, self, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(ready[1]);
    if (error != 0) {
        close(ready[0]);
        return 52;
    }
    char byte;
    ssize_t count = read(ready[0], &byte, 1);
    close(ready[0]);
    return count == 1 && byte == expected ? 0 : 53;
}

static int spawn_holder(const char *self, pid_t *ret_pid) {
    return spawn_captured_mode(self, "--holder", 'R', ret_pid);
}

static int probe_conflict(void) {
    int fd = open_contract_file();
    if (fd < 0) return 10;
    struct flock query = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 16,
    };
    if (fcntl(fd, F_GETLK, &query) != 0 || query.l_type != F_WRLCK ||
        query.l_whence != SEEK_SET || query.l_start != 0 || query.l_len != 16 ||
        query.l_pid <= 0 || query.l_pid == getpid())
        return 11;
    errno = 0;
    if (set_lock(fd, F_SETLK, F_WRLCK, 8, 2) == 0 ||
        (errno != EACCES && errno != EAGAIN))
        return 12;
    if (set_lock(fd, F_SETLK, F_RDLCK, 32, 8) != 0) return 13;
    return close(fd) == 0 ? 0 : 14;
}

static int probe_end_relative(void) {
    int fd = open_contract_file();
    if (fd < 0) return 20;
    struct flock query = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 40,
        .l_len = 8,
    };
    int result = fcntl(fd, F_GETLK, &query);
    close(fd);
    return result == 0 && query.l_type == F_WRLCK && query.l_whence == SEEK_SET &&
                   query.l_start == 40 && query.l_len == 8
               ? 0
               : 21;
}

static int acquire_after_release(int command) {
    int fd = open_contract_file();
    if (fd < 0) return 30;
    int result = set_lock(fd, command, F_WRLCK, 0, 16);
    close(fd);
    return result == 0 ? 0 : 31;
}

static int exit_owner(void) {
    int fd = open_contract_file();
    if (fd < 0) return 40;
    if (set_lock(fd, F_SETLK, F_WRLCK, 0, 16) != 0) return 41;
    // Deliberately rely on process teardown rather than an explicit unlock.
    return 0;
}

static int hold_then_release(void) {
    int fd = open_contract_file();
    if (fd < 0) {
        perror("holder open");
        return 60;
    }
    if (set_lock(fd, F_SETLK, F_WRLCK, 48, 8) != 0) {
        perror("holder F_SETLK");
        return 61;
    }
    if (write(STDOUT_FILENO, "R", 1) != 1) {
        perror("holder ready write");
        return 62;
    }
    close(STDOUT_FILENO);
    usleep(100000);
    if (set_lock(fd, F_SETLK, F_UNLCK, 48, 8) != 0) return 63;
    close(fd);
    return 0;
}

static int wait_for_sibling_lock(void) {
    int fd = open_contract_file();
    if (fd < 0) return 70;
    if (set_lock(fd, F_SETLKW, F_WRLCK, 48, 8) != 0) return 71;
    if (write(STDOUT_FILENO, "W", 1) != 1) return 72;
    return set_lock(fd, F_SETLK, F_UNLCK, 48, 8) == 0 ? 0 : 73;
}

static int deadlock_peer(void) {
    int fd = open_contract_file();
    if (fd < 0) return 80;
    if (set_lock(fd, F_SETLK, F_WRLCK, 8, 8) != 0) return 81;
    if (write(STDOUT_FILENO, "R", 1) != 1) return 82;
    close(STDOUT_FILENO);

    // Give the parent time to register its blocking wait on our range. Our
    // reciprocal wait must then fail with Linux's deadlock errno rather than
    // waiting for the runtime's finite blocking safeguard.
    usleep(50000);
    errno = 0;
    if (set_lock(fd, F_SETLKW, F_WRLCK, 0, 8) == 0 || errno != EDEADLK)
        return 83;
    return close(fd) == 0 ? 0 : 84;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--probe-conflict") == 0)
        return probe_conflict();
    if (argc == 2 && strcmp(argv[1], "--probe-end-relative") == 0)
        return probe_end_relative();
    if (argc == 2 && strcmp(argv[1], "--acquire") == 0)
        return acquire_after_release(F_SETLK);
    if (argc == 2 && strcmp(argv[1], "--blocking-immediate") == 0)
        return acquire_after_release(F_SETLKW);
    if (argc == 2 && strcmp(argv[1], "--exit-owner") == 0)
        return exit_owner();
    if (argc == 2 && strcmp(argv[1], "--holder") == 0)
        return hold_then_release();
    if (argc == 2 && strcmp(argv[1], "--sibling-waiter") == 0)
        return wait_for_sibling_lock();
    if (argc == 2 && strcmp(argv[1], "--deadlock-peer") == 0)
        return deadlock_peer();

    unlink(path);
    int fd = open_contract_file();
    if (fd < 0 || ftruncate(fd, 64) != 0) return 1;
    int duplicate = dup(fd);
    if (duplicate < 0) return 2;
    if (set_lock(fd, F_SETLK, F_WRLCK, 0, 16) != 0) {
        perror("initial F_SETLK");
        return 3;
    }

    struct flock end_relative = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_END,
        .l_start = -16,
        .l_len = -8,
    };
    if (fcntl(fd, F_SETLK, &end_relative) != 0) return 4;
    if (spawn_mode(argv[0], "--probe-conflict") != 0) return 5;
    if (spawn_mode(argv[0], "--probe-end-relative") != 0) return 6;
    puts("record_lock_conflict_and_ranges=yes");

    // Closing any descriptor for this inode releases every POSIX lock held by
    // this process, even though the original descriptor remains open.
    if (close(duplicate) != 0 || spawn_mode(argv[0], "--acquire") != 0) return 7;
    puts("record_lock_any_close_releases=yes");

    if (spawn_mode(argv[0], "--exit-owner") != 0 ||
        spawn_mode(argv[0], "--acquire") != 0)
        return 8;
    puts("record_lock_exit_releases=yes");

    if (spawn_mode(argv[0], "--blocking-immediate") != 0) return 9;
    puts("record_lock_setlkw_immediate=yes");

    if (set_lock(fd, F_SETLK, F_WRLCK, 0, 8) != 0) return 23;
    pid_t deadlock_child;
    if (spawn_captured_mode(argv[0], "--deadlock-peer", 'R', &deadlock_child) != 0)
        return 24;
    if (set_lock(fd, F_SETLKW, F_WRLCK, 8, 8) != 0) return 25;
    int deadlock_status;
    if (waitpid(deadlock_child, &deadlock_status, 0) != deadlock_child ||
        !WIFEXITED(deadlock_status) || WEXITSTATUS(deadlock_status) != 0)
        return 26;
    if (set_lock(fd, F_SETLK, F_UNLCK, 0, 16) != 0) return 27;
    puts("record_lock_setlkw_deadlock=yes");

    int directory_fd = open("/tmp", O_RDONLY | O_DIRECTORY);
    if (directory_fd < 0 ||
        set_lock(directory_fd, F_SETLK, F_RDLCK, 0, 0) != 0 ||
        set_lock(directory_fd, F_SETLK, F_UNLCK, 0, 0) != 0 ||
        close(directory_fd) != 0)
        return 22;
    puts("record_lock_directory_read=yes");

    pid_t holder;
    int holder_error = spawn_holder(argv[0], &holder);
    if (holder_error != 0) {
        fprintf(stderr, "spawn_holder failed: %d errno=%d\n", holder_error, errno);
        return 10;
    }
    struct flock holder_query = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 48,
        .l_len = 8,
    };
    if (fcntl(fd, F_GETLK, &holder_query) != 0 || holder_query.l_type != F_WRLCK ||
        holder_query.l_pid != holder)
        return 11;
    if (set_lock(fd, F_SETLKW, F_WRLCK, 48, 8) != 0) return 12;
    int holder_status;
    if (waitpid(holder, &holder_status, 0) != holder || !WIFEXITED(holder_status) ||
        WEXITSTATUS(holder_status) != 0)
        return 13;
    if (set_lock(fd, F_SETLK, F_UNLCK, 48, 8) != 0) return 14;
    puts("record_lock_setlkw_wakeup=yes");

    if (spawn_holder(argv[0], &holder) != 0) return 15;
    struct sigaction action = {.sa_handler = on_alarm};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL) != 0) return 16;
    struct itimerval timer = {
        .it_value = {.tv_sec = 0, .tv_usec = 20000},
    };
    if (setitimer(ITIMER_REAL, &timer, NULL) != 0) return 17;
    errno = 0;
    if (set_lock(fd, F_SETLKW, F_WRLCK, 48, 8) == 0 || errno != EINTR || !alarm_seen)
        return 18;
    if (waitpid(holder, &holder_status, 0) != holder || !WIFEXITED(holder_status) ||
        WEXITSTATUS(holder_status) != 0)
        return 19;
    puts("record_lock_setlkw_eintr=yes");

    pid_t sibling_waiter;
    if (spawn_holder(argv[0], &holder) != 0 ||
        spawn_captured_mode(argv[0], "--sibling-waiter", 'W', &sibling_waiter) != 0)
        return 20;
    int waiter_status;
    if (waitpid(holder, &holder_status, 0) != holder || !WIFEXITED(holder_status) ||
        WEXITSTATUS(holder_status) != 0 ||
        waitpid(sibling_waiter, &waiter_status, 0) != sibling_waiter ||
        !WIFEXITED(waiter_status) || WEXITSTATUS(waiter_status) != 0)
        return 21;
    puts("record_lock_setlkw_sibling_wakeup=yes");
    puts("record_lock=ok");
    close(fd);
    unlink(path);
    return 0;
}
