/* exec_edge.c -- exec replacement preserves argv and only non-CLOEXEC fds. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void cleanup(const char *keep_path, const char *cloexec_path) {
    if (keep_path) unlink(keep_path);
    if (cloexec_path) unlink(cloexec_path);
}

static int proc_file_contains(const char *path, const char *needle, size_t needle_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    char buffer[4096];
    size_t carried = 0;
    while (needle_len > 0 && needle_len <= sizeof(buffer)) {
        ssize_t length = read(fd, buffer + carried, sizeof(buffer) - carried);
        if (length < 0) break;
        size_t available = carried + (size_t)length;
        for (size_t offset = 0; offset + needle_len <= available; offset++) {
            if (memcmp(buffer + offset, needle, needle_len) == 0) {
                close(fd);
                return 1;
            }
        }
        if (length == 0) break;
        carried = available < needle_len - 1 ? available : needle_len - 1;
        memmove(buffer, buffer + available - carried, carried);
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 7 && strcmp(argv[1], "--after") == 0) {
        int keep_fd = atoi(argv[2]);
        int cloexec_fd = atoi(argv[3]);
        const char *keep_path = argv[4];
        const char *cloexec_path = argv[5];

        errno = 0;
        int keep_open = fcntl(keep_fd, F_GETFD) >= 0;
        errno = 0;
        int cloexec_open_result = fcntl(cloexec_fd, F_GETFD);
        int cloexec_closed = cloexec_open_result == -1 && errno == EBADF;
        int fd_io_ok = 0;
        if (keep_open && lseek(keep_fd, 0, SEEK_SET) >= 0 &&
            write(keep_fd, "ok", 2) == 2 && lseek(keep_fd, 0, SEEK_SET) >= 0) {
            char value[2];
            fd_io_ok = read(keep_fd, value, sizeof(value)) == 2 &&
                       memcmp(value, "ok", sizeof(value)) == 0;
        }

        int argv0_ok = strcmp(argv[0], "custom-exec-argv0") == 0;
        int empty_arg_ok = argv[6][0] == '\0';
        const char proc_argv0[] = "custom-exec-argv0\0";
        const char proc_env[] = "EXEC_EDGE_ENV=committed\0";
        int proc_cmdline_ok = proc_file_contains(
            "/proc/self/cmdline", proc_argv0, sizeof(proc_argv0) - 1);
        int proc_environ_ok = proc_file_contains(
            "/proc/self/environ", proc_env, sizeof(proc_env) - 1);
        int ok = argv0_ok && empty_arg_ok && keep_open && cloexec_closed && fd_io_ok &&
                 proc_cmdline_ok && proc_environ_ok;

        printf("exec_argv0: %s\n", argv0_ok ? "yes" : "no");
        printf("exec_empty_arg: %s\n", empty_arg_ok ? "yes" : "no");
        printf("exec_keep_fd: %s\n", keep_open ? "yes" : "no");
        printf("exec_cloexec_closed: %s\n", cloexec_closed ? "yes" : "no");
        printf("exec_fd_io: %s\n", fd_io_ok ? "yes" : "no");
        printf("exec_proc_cmdline: %s\n", proc_cmdline_ok ? "yes" : "no");
        printf("exec_proc_environ: %s\n", proc_environ_ok ? "yes" : "no");
        printf("exec_replacement: %s\n", ok ? "ok" : "FAIL");

        close(keep_fd);
        if (!cloexec_closed) close(cloexec_fd);
        cleanup(keep_path, cloexec_path);
        return ok ? 0 : 1;
    }

    char keep_path[64];
    char cloexec_path[64];
    snprintf(keep_path, sizeof(keep_path), "exec-edge-keep-%ld.tmp", (long)getpid());
    snprintf(cloexec_path, sizeof(cloexec_path), "exec-edge-cloexec-%ld.tmp", (long)getpid());

    int keep_fd = open(keep_path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    int cloexec_fd = open(cloexec_path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (keep_fd < 0 || cloexec_fd < 0) {
        perror("open");
        if (keep_fd >= 0) close(keep_fd);
        if (cloexec_fd >= 0) close(cloexec_fd);
        cleanup(keep_path, cloexec_path);
        return 1;
    }
    if (fcntl(cloexec_fd, F_SETFD, FD_CLOEXEC) != 0) {
        perror("fcntl(FD_CLOEXEC)");
        close(keep_fd);
        close(cloexec_fd);
        cleanup(keep_path, cloexec_path);
        return 1;
    }
    if (setenv("EXEC_EDGE_ENV", "committed", 1) != 0) {
        perror("setenv");
        close(keep_fd);
        close(cloexec_fd);
        cleanup(keep_path, cloexec_path);
        return 1;
    }

    char keep_fd_arg[32];
    char cloexec_fd_arg[32];
    snprintf(keep_fd_arg, sizeof(keep_fd_arg), "%d", keep_fd);
    snprintf(cloexec_fd_arg, sizeof(cloexec_fd_arg), "%d", cloexec_fd);
    char *replacement_argv[] = {
        "custom-exec-argv0",
        "--after",
        keep_fd_arg,
        cloexec_fd_arg,
        keep_path,
        cloexec_path,
        "",
        NULL,
    };

    execvp(argv[0], replacement_argv);
    perror("execvp");
    close(keep_fd);
    close(cloexec_fd);
    cleanup(keep_path, cloexec_path);
    return 1;
}
