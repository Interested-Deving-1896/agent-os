#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int send_fds(int socket_fd, const int *passed_fds, size_t fd_count,
    char marker) {
    char byte = marker;
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    size_t control_length = CMSG_SPACE(fd_count * sizeof(int));
    unsigned char *control = calloc(1, control_length);
    struct msghdr message;
    struct cmsghdr *header;
    int result;

    if (control == NULL)
        return -1;

    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = control_length;
    header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(fd_count * sizeof(int));
    memcpy(CMSG_DATA(header), passed_fds, fd_count * sizeof(int));
    result = sendmsg(socket_fd, &message, 0) == 1 ? 0 : -1;
    free(control);
    return result;
}

static int send_fd(int socket_fd, int passed_fd) {
    return send_fds(socket_fd, &passed_fd, 1, 'F');
}

static int count_open_fds(void) {
    long limit = sysconf(_SC_OPEN_MAX);
    int count = 0;

    if (limit < 0 || limit > 4096)
        limit = 4096;
    for (int fd = 0; fd < limit; fd++) {
        if (fcntl(fd, F_GETFD) >= 0)
            count++;
    }
    return count;
}

static int message_boundaries(int socket_type) {
    int pair[2];
    char buffer[16] = {0};
    struct iovec iov = { .iov_base = buffer, .iov_len = 3 };
    struct msghdr message;

    if (socketpair(AF_UNIX, socket_type, 0, pair) < 0)
        return 0;
    if (send(pair[0], "first", 5, 0) != 5 ||
        send(pair[0], "second", 6, 0) != 6) {
        close(pair[0]);
        close(pair[1]);
        return 0;
    }
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    int first = recvmsg(pair[1], &message, 0) == 3 &&
        memcmp(buffer, "fir", 3) == 0 &&
        (message.msg_flags & MSG_TRUNC) != 0;
    memset(buffer, 0, sizeof(buffer));
    int second = recv(pair[1], buffer, sizeof(buffer), 0) == 6 &&
        memcmp(buffer, "second", 6) == 0;
    printf("socketpair_%s_truncated_first=%s\n",
        socket_type == SOCK_DGRAM ? "dgram" : "seqpacket",
        first ? "yes" : "no");
    printf("socketpair_%s_second_message=%s\n",
        socket_type == SOCK_DGRAM ? "dgram" : "seqpacket",
        second ? "yes" : "no");
    close(pair[0]);
    close(pair[1]);
    return first && second;
}

static int receive_fd_with_flags(int socket_fd, int flags) {
    char byte = 0;
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(int))];
    } control;
    struct msghdr message;
    struct cmsghdr *header;
    int received_fd = -1;

    memset(&message, 0, sizeof(message));
    memset(&control, 0, sizeof(control));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    if (recvmsg(socket_fd, &message, flags | MSG_CMSG_CLOEXEC) != 1 || byte != 'F')
        return -1;
    header = CMSG_FIRSTHDR(&message);
    if (header == NULL || header->cmsg_level != SOL_SOCKET ||
        header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len < CMSG_LEN(sizeof(int))) {
        errno = EBADMSG;
        return -1;
    }
    memcpy(&received_fd, CMSG_DATA(header), sizeof(received_fd));
    return received_fd;
}

static int receive_fd(int socket_fd) {
    return receive_fd_with_flags(socket_fd, 0);
}

static int transfer_and_close_fd(int original_fd) {
    int channel[2];
    int received_fd;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel) < 0)
        return -1;
    if (send_fd(channel[0], original_fd) < 0) {
        close(channel[0]);
        close(channel[1]);
        close(original_fd);
        return -1;
    }
    close(original_fd);
    received_fd = receive_fd(channel[1]);
    close(channel[0]);
    close(channel[1]);
    return received_fd;
}

static int recvmsg_flag_semantics(void) {
    int stream[2] = {-1, -1};
    int dgram[2] = {-1, -1};
    char buffer[8] = {0};
    struct iovec iov;
    struct msghdr message;
    int ok = 0;
    int peek_ok = 0;
    int waitall_ok = 0;
    int trunc_ok = 0;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, stream) < 0 ||
        socketpair(AF_UNIX, SOCK_DGRAM, 0, dgram) < 0)
        goto done;
    if (send(stream[0], "hello", 5, 0) != 5)
        goto done;

    memset(&message, 0, sizeof(message));
    iov.iov_base = buffer;
    iov.iov_len = 2;
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    peek_ok = recvmsg(stream[1], &message, MSG_PEEK) == 2 &&
        memcmp(buffer, "he", 2) == 0;
    if (!peek_ok)
        goto done;

    memset(buffer, 0, sizeof(buffer));
    iov.iov_len = 5;
    waitall_ok = recvmsg(stream[1], &message, MSG_WAITALL) == 5 &&
        memcmp(buffer, "hello", 5) == 0;
    if (!waitall_ok)
        goto done;

    if (send(dgram[0], "abcdef", 6, 0) != 6)
        goto done;
    memset(buffer, 0, sizeof(buffer));
    memset(&message, 0, sizeof(message));
    iov.iov_base = buffer;
    iov.iov_len = 2;
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    ssize_t trunc_received = recvmsg(dgram[1], &message, MSG_TRUNC);
    trunc_ok = trunc_received == 6 && memcmp(buffer, "ab", 2) == 0 &&
        (message.msg_flags & MSG_TRUNC) != 0;
    if (!trunc_ok)
        goto done;

    ok = 1;
done:
    printf("socketpair_msg_peek=%s\n", peek_ok ? "yes" : "no");
    printf("socketpair_msg_waitall=%s\n", waitall_ok ? "yes" : "no");
    printf("socketpair_msg_trunc=%s\n", trunc_ok ? "yes" : "no");
    if (stream[0] >= 0)
        close(stream[0]);
    if (stream[1] >= 0)
        close(stream[1]);
    if (dgram[0] >= 0)
        close(dgram[0]);
    if (dgram[1] >= 0)
        close(dgram[1]);
    return ok;
}

static int transfer_pending_tcp_socket(void) {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
        return 0;
    socket_fd = transfer_and_close_fd(socket_fd);
    if (socket_fd < 0)
        return 0;

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    int ok = bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) == 0 &&
        listen(socket_fd, 1) == 0;
    close(socket_fd);
    return ok;
}

static int transfer_tcp_listener_and_connection(int *peek_shared) {
    int listener = -1;
    int client = -1;
    int accepted = -1;
    int peeked = -1;
    int channel[2] = {-1, -1};
    int ok = 0;
    int listener_transferred = 0;
    int client_connected = 0;
    int connection_accepted = 0;
    int right_peeked = 0;
    int right_consumed = 0;
    int data_sent = 0;
    int peek_read = 0;
    int consumed_read = 0;
    struct sockaddr_in address;
    socklen_t address_len = sizeof(address);
    char buffer[16] = {0};

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
        goto done;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listener, 1) < 0 ||
        getsockname(listener, (struct sockaddr *)&address, &address_len) < 0)
        goto done;
    listener = transfer_and_close_fd(listener);
    if (listener < 0)
        goto done;
    listener_transferred = 1;

    client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0 ||
        connect(client, (struct sockaddr *)&address, sizeof(address)) < 0)
        goto done;
    client_connected = 1;
    accepted = accept(listener, NULL, NULL);
    if (accepted < 0)
        goto done;
    connection_accepted = 1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel) < 0 ||
        send_fd(channel[0], accepted) < 0)
        goto done;
    peeked = receive_fd_with_flags(channel[1], MSG_PEEK);
    if (peeked >= 0)
        right_peeked = 1;
    close(accepted);
    accepted = -1;
    if (peeked < 0 || (accepted = receive_fd(channel[1])) < 0)
        goto done;
    right_consumed = 1;
    data_sent = send(client, "connected", 9, 0) == 9;
    if (!data_sent)
        goto done;
    peek_read = recv(peeked, buffer, 4, MSG_WAITALL) == 4;
    if (!peek_read)
        goto done;
    consumed_read = recv(accepted, buffer + 4, 5, MSG_WAITALL) == 5;
    if (!consumed_read || memcmp(buffer, "connected", 9) != 0)
        goto done;
    *peek_shared = 1;
    ok = 1;
done:
    printf("scm_rights_tcp_listener_transferred=%s\n",
        listener_transferred ? "yes" : "no");
    printf("scm_rights_tcp_client_connected=%s\n",
        client_connected ? "yes" : "no");
    printf("scm_rights_tcp_connection_accepted=%s\n",
        connection_accepted ? "yes" : "no");
    printf("scm_rights_tcp_right_peeked=%s\n", right_peeked ? "yes" : "no");
    printf("scm_rights_tcp_right_consumed=%s\n",
        right_consumed ? "yes" : "no");
    printf("scm_rights_tcp_data_sent=%s\n", data_sent ? "yes" : "no");
    printf("scm_rights_tcp_peek_read=%s\n", peek_read ? "yes" : "no");
    printf("scm_rights_tcp_consumed_read=%s\n",
        consumed_read ? "yes" : "no");
    if (listener >= 0)
        close(listener);
    if (client >= 0)
        close(client);
    if (accepted >= 0)
        close(accepted);
    if (peeked >= 0)
        close(peeked);
    if (channel[0] >= 0)
        close(channel[0]);
    if (channel[1] >= 0)
        close(channel[1]);
    return ok;
}

static int transfer_udp_socket(void) {
    int receiver = -1;
    int sender = -1;
    int ok = 0;
    int bound = 0;
    int transferred = 0;
    int sent = 0;
    int readable = 0;
    int received = 0;
    struct sockaddr_in address;
    socklen_t address_len = sizeof(address);
    char buffer[16] = {0};

    receiver = socket(AF_INET, SOCK_DGRAM, 0);
    if (receiver < 0)
        goto done;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(receiver, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        getsockname(receiver, (struct sockaddr *)&address, &address_len) < 0)
        goto done;
    bound = 1;
    receiver = transfer_and_close_fd(receiver);
    if (receiver < 0)
        goto done;
    transferred = 1;
    sender = socket(AF_INET, SOCK_DGRAM, 0);
    if (sender < 0 ||
        sendto(sender, "datagram", 8, 0,
            (struct sockaddr *)&address, sizeof(address)) != 8)
        goto done;
    sent = 1;
    struct pollfd poll_fd = { .fd = receiver, .events = POLLIN };
    readable = poll(&poll_fd, 1, 1000) == 1;
    if (!readable)
        goto done;
    received = recv(receiver, buffer, sizeof(buffer), 0) == 8 &&
        memcmp(buffer, "datagram", 8) == 0;
    if (!received)
        goto done;
    ok = 1;
done:
    printf("scm_rights_udp_bound=%s\n", bound ? "yes" : "no");
    printf("scm_rights_udp_transferred=%s\n", transferred ? "yes" : "no");
    printf("scm_rights_udp_sent=%s\n", sent ? "yes" : "no");
    printf("scm_rights_udp_readable=%s\n", readable ? "yes" : "no");
    printf("scm_rights_udp_received=%s\n", received ? "yes" : "no");
    if (receiver >= 0)
        close(receiver);
    if (sender >= 0)
        close(sender);
    return ok;
}

static int child_main(const char *socket_arg, const char *fd_arg) {
    int socket_fd = atoi(socket_arg);
    int passed_fd = atoi(fd_arg);
    if (send_fd(socket_fd, passed_fd) < 0) {
        perror("child sendmsg");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int ok = 1;

    if (argc == 4 && strcmp(argv[1], "--child") == 0)
        return child_main(argv[2], argv[3]);

    int invalid_pair[2];
    errno = 0;
    int invalid_domain = socketpair(AF_INET, SOCK_STREAM, 0,
        invalid_pair) == -1 && errno == EOPNOTSUPP;
    errno = 0;
    int invalid_flags = socketpair(AF_UNIX, SOCK_STREAM | 0x0100, 0,
        invalid_pair) == -1 && errno == EINVAL;
    errno = 0;
    int invalid_type = socketpair(AF_UNIX, 7, 0,
        invalid_pair) == -1 && errno == ESOCKTNOSUPPORT;
    errno = 0;
    int invalid_protocol = socketpair(AF_UNIX, SOCK_STREAM, 2,
        invalid_pair) == -1 && errno == EPROTONOSUPPORT;
    errno = 0;
    int null_result = socketpair(AF_UNIX, SOCK_STREAM, 0, NULL);
    int null_vector = null_result == -1 && errno == EFAULT;
    printf("socketpair_invalid_domain=%s\n", invalid_domain ? "yes" : "no");
    printf("socketpair_invalid_flags=%s\n", invalid_flags ? "yes" : "no");
    printf("socketpair_invalid_type=%s\n", invalid_type ? "yes" : "no");
    printf("socketpair_invalid_protocol=%s\n",
        invalid_protocol ? "yes" : "no");
    printf("socketpair_null_vector=%s\n", null_vector ? "yes" : "no");
    ok &= invalid_domain && invalid_flags && invalid_type &&
        invalid_protocol && null_vector;

    int flagged[2];
    if (socketpair(AF_UNIX,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, flagged) < 0) {
        perror("socketpair(flags)");
        return 1;
    }
    int first_nonblock = (fcntl(flagged[0], F_GETFL) & O_NONBLOCK) != 0;
    int second_nonblock = (fcntl(flagged[1], F_GETFL) & O_NONBLOCK) != 0;
    int first_cloexec = (fcntl(flagged[0], F_GETFD) & FD_CLOEXEC) != 0;
    int second_cloexec = (fcntl(flagged[1], F_GETFD) & FD_CLOEXEC) != 0;
    printf("socketpair_nonblock=%s\n",
        first_nonblock && second_nonblock ? "yes" : "no");
    printf("socketpair_cloexec=%s\n",
        first_cloexec && second_cloexec ? "yes" : "no");
    ok &= first_nonblock && second_nonblock && first_cloexec && second_cloexec;
    close(flagged[0]);
    close(flagged[1]);

    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
        perror("socketpair");
        return 1;
    }
    struct iovec invalid_iov = { .iov_base = NULL, .iov_len = 1 };
    struct msghdr invalid_message;
    memset(&invalid_message, 0, sizeof(invalid_message));
    invalid_message.msg_iov = &invalid_iov;
    invalid_message.msg_iovlen = 1;
    errno = 0;
    int iov_efault = sendmsg(pair[0], &invalid_message, 0) == -1 &&
        errno == EFAULT;
    printf("sendmsg_iov_efault=%s\n", iov_efault ? "yes" : "no");
    ok &= iov_efault;
    if (write(pair[0], "left", 4) != 4) {
        perror("socketpair write left");
        return 1;
    }
    struct pollfd poll_fd = { .fd = pair[1], .events = POLLIN };
    int readable = poll(&poll_fd, 1, 1000) == 1 &&
        (poll_fd.revents & POLLIN) != 0;
    char buffer[16] = {0};
    int left_to_right = read(pair[1], buffer, sizeof(buffer)) == 4 &&
        memcmp(buffer, "left", 4) == 0;
    int right_to_left = write(pair[1], "right", 5) == 5 &&
        read(pair[0], buffer, sizeof(buffer)) == 5 &&
        memcmp(buffer, "right", 5) == 0;
    printf("socketpair_poll=%s\n", readable ? "yes" : "no");
    printf("socketpair_bidirectional=%s\n",
        left_to_right && right_to_left ? "yes" : "no");
    ok &= readable && left_to_right && right_to_left;

    if (shutdown(pair[0], SHUT_WR) < 0) {
        perror("socketpair shutdown");
        return 1;
    }
    int shutdown_eof = read(pair[1], buffer, sizeof(buffer)) == 0;
    printf("socketpair_shutdown_eof=%s\n", shutdown_eof ? "yes" : "no");
    ok &= shutdown_eof;
    close(pair[0]);
    close(pair[1]);

    int dgram_boundaries = message_boundaries(SOCK_DGRAM);
    int seqpacket_boundaries = message_boundaries(SOCK_SEQPACKET);
    int recvmsg_flags = recvmsg_flag_semantics();
    printf("socketpair_dgram_boundaries=%s\n",
        dgram_boundaries ? "yes" : "no");
    printf("socketpair_seqpacket_boundaries=%s\n",
        seqpacket_boundaries ? "yes" : "no");
    printf("socketpair_recvmsg_flags=%s\n", recvmsg_flags ? "yes" : "no");
    ok &= dgram_boundaries && seqpacket_boundaries && recvmsg_flags;

    int trunc_pair[2];
    int trunc_pipe_a[2];
    int trunc_pipe_b[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, trunc_pair) < 0 ||
        pipe(trunc_pipe_a) < 0 || pipe(trunc_pipe_b) < 0) {
        perror("SCM_RIGHTS truncation setup");
        return 1;
    }
    int trunc_fds[2] = { trunc_pipe_a[0], trunc_pipe_b[0] };
    if (send_fds(trunc_pair[0], trunc_fds, 2, 'T') < 0) {
        perror("SCM_RIGHTS truncation sendmsg");
        return 1;
    }
    int before_truncated_receive = count_open_fds();
    char trunc_byte = 0;
    struct iovec trunc_iov = { .iov_base = &trunc_byte, .iov_len = 1 };
    struct msghdr trunc_message;
    memset(&trunc_message, 0, sizeof(trunc_message));
    trunc_message.msg_iov = &trunc_iov;
    trunc_message.msg_iovlen = 1;
    int trunc_payload = recvmsg(trunc_pair[1], &trunc_message, 0) == 1 &&
        trunc_byte == 'T';
    int trunc_flag = (trunc_message.msg_flags & MSG_CTRUNC) != 0;
    int no_disclosed_fds = count_open_fds() == before_truncated_receive;
    printf("scm_rights_truncated_payload=%s\n",
        trunc_payload ? "yes" : "no");
    printf("scm_rights_ctrunc=%s\n", trunc_flag ? "yes" : "no");
    printf("scm_rights_truncated_fds_closed=%s\n",
        no_disclosed_fds ? "yes" : "no");
    ok &= trunc_payload && trunc_flag && no_disclosed_fds;
    close(trunc_pair[0]);
    close(trunc_pair[1]);
    close(trunc_pipe_a[0]);
    close(trunc_pipe_a[1]);
    close(trunc_pipe_b[0]);
    close(trunc_pipe_b[1]);

    int pass_pair[2];
    int payload_pipe[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pass_pair) < 0 ||
        pipe(payload_pipe) < 0) {
        perror("descriptor passing setup");
        return 1;
    }
    char socket_arg[32];
    char fd_arg[32];
    snprintf(socket_arg, sizeof(socket_arg), "%d", pass_pair[1]);
    snprintf(fd_arg, sizeof(fd_arg), "%d", payload_pipe[0]);
    char *child_argv[] = {
        argv[0], "--child", socket_arg, fd_arg, NULL,
    };
    pid_t child;
    int spawn_error = posix_spawnp(&child, argv[0], NULL, NULL,
        child_argv, environ);
    if (spawn_error != 0) {
        errno = spawn_error;
        perror("posix_spawnp socketpair child");
        return 1;
    }

    close(pass_pair[1]);
    close(payload_pipe[0]);
    if (write(payload_pipe[1], "passed", 6) != 6) {
        perror("write descriptor payload");
        return 1;
    }
    close(payload_pipe[1]);

    int received_fd = receive_fd(pass_pair[0]);
    int received_cloexec = received_fd >= 0 &&
        (fcntl(received_fd, F_GETFD) & FD_CLOEXEC) != 0;
    memset(buffer, 0, sizeof(buffer));
    int received_payload = received_fd >= 0 &&
        read(received_fd, buffer, sizeof(buffer)) == 6 &&
        memcmp(buffer, "passed", 6) == 0;
    int status = 0;
    int child_ok = waitpid(child, &status, 0) == child &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("scm_rights_cross_process=%s\n",
        received_payload && child_ok ? "yes" : "no");
    printf("scm_rights_child_exit=%s\n", child_ok ? "yes" : "no");
    printf("scm_rights_child_received_fd=%s\n",
        received_fd >= 0 ? "yes" : "no");
    printf("scm_rights_child_payload=%s\n", received_payload ? "yes" : "no");
    printf("scm_rights_cloexec=%s\n", received_cloexec ? "yes" : "no");
    ok &= received_payload && child_ok && received_cloexec;
    if (received_fd >= 0)
        close(received_fd);
    close(pass_pair[0]);

    int pending_tcp_transfer = transfer_pending_tcp_socket();
    int hostnet_peek_shared = 0;
    int tcp_transfer = transfer_tcp_listener_and_connection(&hostnet_peek_shared);
    int udp_transfer = transfer_udp_socket();
    printf("scm_rights_pending_tcp_socket=%s\n",
        pending_tcp_transfer ? "yes" : "no");
    printf("scm_rights_tcp_listener_connection=%s\n",
        tcp_transfer ? "yes" : "no");
    printf("scm_rights_hostnet_peek_shared=%s\n",
        hostnet_peek_shared ? "yes" : "no");
    printf("scm_rights_udp_socket=%s\n", udp_transfer ? "yes" : "no");
    ok &= pending_tcp_transfer && tcp_transfer && hostnet_peek_shared && udp_transfer;

    printf("socketpair_rights=%s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}
