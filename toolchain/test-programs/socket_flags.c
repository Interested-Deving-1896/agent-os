#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int has_status_flag(int fd, int flag) {
    int value = fcntl(fd, F_GETFL);
    return value >= 0 && (value & flag) != 0;
}

static int has_fd_flag(int fd, int flag) {
    int value = fcntl(fd, F_GETFD);
    return value >= 0 && (value & flag) != 0;
}

int main(void) {
    int ok = 1;
    int flagged = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (flagged < 0) {
        perror("socket(flags)");
        return 1;
    }

    int socket_nonblock = has_status_flag(flagged, O_NONBLOCK);
    int socket_cloexec = has_fd_flag(flagged, FD_CLOEXEC);
    printf("socket_nonblock=%s\n", socket_nonblock ? "yes" : "no");
    printf("socket_cloexec=%s\n", socket_cloexec ? "yes" : "no");
    ok &= socket_nonblock && socket_cloexec;
    close(flagged);

    errno = 0;
    int invalid_socket = socket(AF_INET, SOCK_STREAM | 0x20000000, 0);
    int invalid_socket_errno = errno;
    if (invalid_socket >= 0)
        close(invalid_socket);
    int invalid_socket_einval =
        invalid_socket == -1 && invalid_socket_errno == EINVAL;
    printf("socket_invalid_flag_einval=%s\n",
           invalid_socket_einval ? "yes" : "no");
    ok &= invalid_socket_einval;

    /* Linux looks up the descriptor before validating accept4 flags. */
    errno = 0;
    int badfd_accept = accept4(-1, NULL, NULL, 0x20000000);
    int badfd_accept_errno = errno;
    int badfd_accept_ebadf =
        badfd_accept == -1 && badfd_accept_errno == EBADF;
    printf("accept4_badfd_ebadf=%s\n",
           badfd_accept_ebadf ? "yes" : "no");
    ok &= badfd_accept_ebadf;

    /* Establish a loopback connection in one process. TCP connect may finish
     * before accept because the listening socket's backlog owns the pending
     * connection, so no fork/thread is needed by the WASM fixture. */
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        perror("socket(listener)");
        return 1;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listener, 1) < 0) {
        perror("listen(loopback)");
        close(listener);
        return 1;
    }

    errno = 0;
    int invalid_accept = accept4(listener, NULL, NULL, 0x20000000);
    int invalid_accept_errno = errno;
    int invalid_accept_einval =
        invalid_accept == -1 && invalid_accept_errno == EINVAL;
    printf("accept4_invalid_flag_einval=%s\n",
           invalid_accept_einval ? "yes" : "no");
    ok &= invalid_accept_einval;

    socklen_t address_len = sizeof(address);
    if (getsockname(listener, (struct sockaddr *)&address, &address_len) < 0) {
        perror("getsockname(listener)");
        close(listener);
        return 1;
    }

    int client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0 ||
        connect(client, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("connect(loopback)");
        if (client >= 0)
            close(client);
        close(listener);
        return 1;
    }

    int accepted = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (accepted < 0) {
        perror("accept4(flags)");
        close(client);
        close(listener);
        return 1;
    }

    int accept_nonblock = has_status_flag(accepted, O_NONBLOCK);
    int accept_cloexec = has_fd_flag(accepted, FD_CLOEXEC);
    printf("accept4_nonblock=%s\n", accept_nonblock ? "yes" : "no");
    printf("accept4_cloexec=%s\n", accept_cloexec ? "yes" : "no");
    ok &= accept_nonblock && accept_cloexec;

    /* dup/F_DUPFD share the socket open-file description (including status
     * flags), but each descriptor has independent FD_CLOEXEC state. */
    int duplicate = dup(accepted);
    int duplicate_ok = duplicate >= 0 && !has_fd_flag(duplicate, FD_CLOEXEC) &&
        has_status_flag(duplicate, O_NONBLOCK);
    if (duplicate >= 0 && fcntl(duplicate, F_SETFL, 0) == 0)
        duplicate_ok &= !has_status_flag(accepted, O_NONBLOCK);
    if (duplicate >= 0 && fcntl(duplicate, F_SETFD, FD_CLOEXEC) == 0)
        duplicate_ok &= has_fd_flag(duplicate, FD_CLOEXEC) &&
            has_fd_flag(accepted, FD_CLOEXEC);
    printf("socket_dup_shared_ofd=%s\n", duplicate_ok ? "yes" : "no");
    ok &= duplicate_ok;

    int exact = duplicate >= 0 ? dup2(duplicate, 42) : -1;
    int dup2_exact = exact == 42 && !has_fd_flag(exact, FD_CLOEXEC);
    printf("socket_dup2_exact=%s\n", dup2_exact ? "yes" : "no");
    ok &= dup2_exact;

    int minimum = exact >= 0 ? fcntl(exact, F_DUPFD, 50) : -1;
    int dup_min = minimum >= 50 && !has_fd_flag(minimum, FD_CLOEXEC);
    printf("socket_f_dupfd_min=%s\n", dup_min ? "yes" : "no");
    ok &= dup_min;

    /* Closing the source must leave each duplicate usable. */
    close(accepted);
    const char byte = 'x';
    char received = 0;
    int independent_close = exact >= 0 && send(client, &byte, 1, 0) == 1 &&
        recv(exact, &received, 1, 0) == 1 && received == byte;
    printf("socket_dup_independent_close=%s\n",
           independent_close ? "yes" : "no");
    ok &= independent_close;

    if (duplicate >= 0)
        close(duplicate);
    if (exact >= 0)
        close(exact);
    if (minimum >= 0)
        close(minimum);
    close(client);
    close(listener);

    printf("socket_flags=%s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}
