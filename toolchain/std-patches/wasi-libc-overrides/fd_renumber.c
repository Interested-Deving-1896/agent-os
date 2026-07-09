/**
 * Preserve wasi-libc's internal fd-renumber operation after replacing close().
 *
 * Upstream currently emits close() and __wasilibc_fd_renumber() in the same
 * archive member.  The override installer must remove that member so the owned
 * close()/CLOEXEC implementation wins, which would otherwise also remove the
 * symbol used by freopen().
 */

#include <errno.h>
#include <wasi/api.h>
#include <wasi/libc.h>

int __wasilibc_fd_renumber(int fd, int newfd) {
    __wasilibc_populate_preopens();

    __wasi_errno_t error = __wasi_fd_renumber(
        (__wasi_fd_t)fd, (__wasi_fd_t)newfd);
    if (error != 0) {
        errno = (int)error;
        return -1;
    }
    return 0;
}
