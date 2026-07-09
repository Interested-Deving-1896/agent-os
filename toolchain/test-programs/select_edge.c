/* select_edge.c -- select() descriptor-set and limit Linux parity. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <unistd.h>

#ifdef __wasi__
_Static_assert(FD_SETSIZE == 8192,
               "WASM select regression must use the large caller fd_set");
#endif

int main(void) {
    const char *path = "select-edge.tmp";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    if (write(fd, "x", 1) != 1 || lseek(fd, 0, SEEK_SET) < 0) {
        perror("prepare");
        close(fd);
        unlink(path);
        return 1;
    }

    int high_fd = fcntl(fd, F_DUPFD, 512);
    if (high_fd < 512 || high_fd >= FD_SETSIZE) {
        perror("fcntl(F_DUPFD, 512)");
        close(fd);
        unlink(path);
        return 1;
    }
    int cloexec_fd = fcntl(fd, F_DUPFD_CLOEXEC, 513);
    int cloexec_ok = cloexec_fd >= 513 && cloexec_fd < FD_SETSIZE &&
        (fcntl(cloexec_fd, F_GETFD) & FD_CLOEXEC) != 0;
    if (!cloexec_ok) {
        perror("fcntl(F_DUPFD_CLOEXEC, 513)");
        if (cloexec_fd >= 0) close(cloexec_fd);
        close(high_fd);
        close(fd);
        unlink(path);
        return 1;
    }

    errno = 0;
    int ceiling_fd = fcntl(fd, F_DUPFD, 1 << 20);
    int ceiling_einval_ok = ceiling_fd == -1 && errno == EINVAL;
    if (ceiling_fd >= 0) close(ceiling_fd);

    /* Compile the WASM fixture with an overrideable 8192-entry fd_set to prove
     * libc remains prefix-compatible. Keep these caller-owned sets off the
     * bounded WASM stack. */
    fd_set *readfds = calloc(1, sizeof(*readfds));
    fd_set *writefds = calloc(1, sizeof(*writefds));
    if (!readfds || !writefds) {
        perror("calloc");
        free(readfds);
        free(writefds);
        close(cloexec_fd);
        close(high_fd);
        close(fd);
        unlink(path);
        return 1;
    }
    FD_ZERO(readfds);
    FD_ZERO(writefds);
    FD_SET(high_fd, readfds);
    FD_SET(high_fd, writefds);
    /* glibc accepts and normalizes nonnegative tv_usec values above one
     * second. A ready descriptor should return immediately with most of the
     * normalized second still remaining. */
    struct timeval timeout = {0, 1000000};

    /* The WASM caller's fd_set is larger than libc's compiled default, but
     * Linux nfds is still the highest requested descriptor plus one. */
    int ready = select(high_fd + 1, readfds, writefds, NULL, &timeout);
    int read_ready = FD_ISSET(high_fd, readfds) != 0;
    int write_ready = FD_ISSET(high_fd, writefds) != 0;
    int large_fdset_ok = FD_SETSIZE >= 1024;
    int timeout_normalized_ok = timeout.tv_sec >= 0 && timeout.tv_sec <= 1 &&
        timeout.tv_usec >= 0 && timeout.tv_usec < 1000000 &&
        (timeout.tv_sec > 0 || timeout.tv_usec > 500000);

    struct timeval expiring_timeout = {0, 5000};
    int expiring_result =
        select(0, NULL, NULL, NULL, &expiring_timeout);
    int timeout_expired_ok = expiring_result == 0 &&
        expiring_timeout.tv_sec == 0 && expiring_timeout.tv_usec == 0;

    struct timeval invalid_timeout = {0, -1};
    errno = 0;
    int invalid_timeout_result =
        select(0, NULL, NULL, NULL, &invalid_timeout);
    int invalid_timeout_ok = invalid_timeout_result == -1 && errno == EINVAL;

    errno = 0;
    struct timeval zero_timeout = {0, 0};
    int invalid_nfds_result = select(-1, NULL, NULL, NULL, &zero_timeout);
    int invalid_nfds_ok = invalid_nfds_result == -1 && errno == EINVAL;

    /* Bits at or above nfds are ignored and cleared from the result. */
    FD_ZERO(readfds);
    FD_SET(fd, readfds);
    struct timeval ignored_timeout = {0, 0};
    int ignored_result = select(fd, readfds, NULL, NULL, &ignored_timeout);
    int above_nfds_ignored_ok = ignored_result == 0 && !FD_ISSET(fd, readfds);

    /* A closed descriptor below nfds makes the whole call fail with EBADF. */
    int stale_fd = dup(fd);
    int ebadf_ok = 0;
    if (stale_fd >= 0 && close(stale_fd) == 0) {
        FD_ZERO(readfds);
        FD_SET(stale_fd, readfds);
        struct timeval ebadf_timeout = {0, 0};
        errno = 0;
        int ebadf_result =
            select(stale_fd + 1, readfds, NULL, NULL, &ebadf_timeout);
        ebadf_ok = ebadf_result == -1 && errno == EBADF;
    }

    /* Linux accepts a huge numeric nfds for sparse and empty sets. These calls
     * also prove the implementation does not perform an O(nfds) scan. */
    FD_ZERO(readfds);
    struct timeval huge_empty_timeout = {0, 0};
    errno = 0;
    int huge_empty_result =
        select(INT_MAX, readfds, NULL, NULL, &huge_empty_timeout);
    int huge_empty_ok = huge_empty_result == 0;

    FD_ZERO(readfds);
    FD_SET(fd, readfds);
    struct timeval huge_sparse_timeout = {0, 0};
    errno = 0;
    int huge_sparse_result =
        select(INT_MAX, readfds, NULL, NULL, &huge_sparse_timeout);
    int huge_sparse_ok =
        huge_sparse_result == 1 && FD_ISSET(fd, readfds);

    int ok = ready == 2 && read_ready && write_ready && large_fdset_ok &&
        timeout_normalized_ok && timeout_expired_ok && invalid_timeout_ok &&
        invalid_nfds_ok && above_nfds_ignored_ok && ebadf_ok &&
        huge_empty_ok && huge_sparse_ok && cloexec_ok && ceiling_einval_ok;

    printf("select_ready_count: %d\n", ready);
    printf("select_read_ready: %s\n", read_ready ? "yes" : "no");
    printf("select_write_ready: %s\n", write_ready ? "yes" : "no");
    printf("select_large_fdset: %s\n", large_fdset_ok ? "yes" : "no");
    printf("select_high_fd: %s\n", high_fd >= 512 ? "yes" : "no");
    printf("select_cloexec_fd: %s\n", cloexec_ok ? "yes" : "no");
    printf("select_fd_ceiling_einval: %s\n",
           ceiling_einval_ok ? "yes" : "no");
    printf("select_timeout_normalized: %s\n",
           timeout_normalized_ok ? "yes" : "no");
    printf("select_timeout_expired: %s\n",
           timeout_expired_ok ? "yes" : "no");
    printf("select_invalid_timeout: %s\n",
           invalid_timeout_ok ? "yes" : "no");
    printf("select_invalid_nfds: %s\n", invalid_nfds_ok ? "yes" : "no");
    printf("select_above_nfds_ignored: %s\n",
           above_nfds_ignored_ok ? "yes" : "no");
    printf("select_ebadf: %s\n", ebadf_ok ? "yes" : "no");
    printf("select_huge_empty: %s\n", huge_empty_ok ? "yes" : "no");
    printf("select_huge_sparse: %s\n", huge_sparse_ok ? "yes" : "no");
    printf("select_multi_set: %s\n", ok ? "ok" : "FAIL");

    free(readfds);
    free(writefds);
    close(cloexec_fd);
    close(high_fd);
    close(fd);
    unlink(path);
    return ok ? 0 : 1;
}
