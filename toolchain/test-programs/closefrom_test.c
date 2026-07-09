#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    static const char path[] = "/tmp/agentos-closefrom-test";
    static const char contents[] = "closefrom-path-ok";
    int seed_fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (seed_fd < 0 || write(seed_fd, contents, sizeof(contents)) != sizeof(contents) ||
        close(seed_fd) < 0) {
        perror("seed closefrom path");
        return 1;
    }

    int low_fd = open(path, O_RDONLY);
    if (low_fd < 0) {
        perror("open low fd");
        return 1;
    }
    int high_fd = fcntl(low_fd, F_DUPFD, 512);
    if (high_fd < 0) {
        perror("F_DUPFD");
        return 1;
    }

    closefrom(STDERR_FILENO + 1);
    errno = 0;
    int low_closed = fcntl(low_fd, F_GETFD) == -1 && errno == EBADF;
    errno = 0;
    int high_closed = fcntl(high_fd, F_GETFD) == -1 && errno == EBADF;
    int stdio_open = fcntl(STDOUT_FILENO, F_GETFD) >= 0;

    char buffer[sizeof(contents)] = {0};
    int reopened_fd = open(path, O_RDONLY);
    int path_access = reopened_fd >= 0 &&
        read(reopened_fd, buffer, sizeof(buffer)) == sizeof(contents) &&
        memcmp(buffer, contents, sizeof(contents)) == 0;
    if (reopened_fd >= 0)
        close(reopened_fd);
    unlink(path);

    int closed = low_closed && high_closed && stdio_open && path_access;
    printf("closefrom_closed=%s\n", closed ? "yes" : "no");
    printf("closefrom_stdio_open=%s\n", stdio_open ? "yes" : "no");
    printf("closefrom_path_access=%s\n", path_access ? "yes" : "no");
    return closed ? 0 : 1;
}
