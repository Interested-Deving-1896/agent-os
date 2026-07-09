/* unix_socket.c — AF_UNIX server: bind, listen, accept one connection, recv, send "pong", close */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>

#ifdef __has_include
#  if __has_include(<sys/un.h>)
#    include <sys/un.h>
#  endif
#endif

#ifndef AF_UNIX
#  define AF_UNIX 1
#endif

#ifndef AF_LOCAL
#  define AF_LOCAL AF_UNIX
#endif

#ifndef offsetof
#  define offsetof(type, member) __builtin_offsetof(type, member)
#endif

/* Fallback if sys/un.h was not available */
#ifndef SUN_LEN
struct sockaddr_un {
    sa_family_t sun_family;
    char sun_path[108];
};
#define SUN_LEN(su) (offsetof(struct sockaddr_un, sun_path) + strlen((su)->sun_path))
#endif

extern char **environ;

static const char abstract_name[] = "agentos-abstract-contract";

static int contract_failures;

static void contract_check(const char *name, int passed) {
    printf("%s=%s\n", name, passed ? "yes" : "NO");
    if (!passed) contract_failures++;
}

static int failed_with(int result, int expected_errno) {
    return result == -1 && errno == expected_errno;
}

static socklen_t unique_abstract_address(struct sockaddr_un *addr,
                                         unsigned int sequence) {
    int length;

    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    length = snprintf(addr->sun_path + 1, sizeof(addr->sun_path) - 1,
                      "aos-contract-%ld-%u", (long)getpid(), sequence);
    if (length < 0 || (size_t)length >= sizeof(addr->sun_path) - 1)
        return 0;
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + length);
}

static int abstract_listener(unsigned int sequence, int backlog,
                             struct sockaddr_un *addr, socklen_t *addrlen) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    *addrlen = unique_abstract_address(addr, sequence);
    if (*addrlen == 0 || bind(fd, (struct sockaddr *)addr, *addrlen) < 0 ||
        listen(fd, backlog) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int abstract_connect(const struct sockaddr_un *addr,
                            socklen_t addrlen) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (const struct sockaddr *)addr, addrlen) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int lowercase_hex_name(const unsigned char *bytes, size_t length) {
    size_t i;
    for (i = 0; i < length; i++) {
        if (!((bytes[i] >= '0' && bytes[i] <= '9') ||
              (bytes[i] >= 'a' && bytes[i] <= 'f')))
            return 0;
    }
    return 1;
}

static void address_length_contract(void) {
    const socklen_t path_offset =
        (socklen_t)offsetof(struct sockaddr_un, sun_path);
    struct sockaddr_un family_only;
    struct sockaddr_storage returned;
    socklen_t returned_len;
    int fd;

    memset(&family_only, 0, sizeof(family_only));
    family_only.sun_family = AF_UNIX;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0 &&
        bind(fd, (struct sockaddr *)&family_only, path_offset) == 0) {
        memset(&returned, 0xa5, sizeof(returned));
        returned_len = sizeof(returned);
        contract_check("unix_family_only_autobind",
            getsockname(fd, (struct sockaddr *)&returned, &returned_len) == 0 &&
            returned_len == path_offset + 6 &&
            ((struct sockaddr_un *)&returned)->sun_family == AF_UNIX &&
            ((struct sockaddr_un *)&returned)->sun_path[0] == '\0' &&
            lowercase_hex_name(
                (const unsigned char *)
                    ((struct sockaddr_un *)&returned)->sun_path + 1,
                5));
    } else {
        contract_check("unix_family_only_autobind", 0);
    }
    if (fd >= 0) close(fd);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    contract_check("unix_family_only_connect_einval",
        fd >= 0 && failed_with(
            connect(fd, (struct sockaddr *)&family_only, path_offset), EINVAL));
    if (fd >= 0) close(fd);

    {
        struct sockaddr_un maximum;
        char path[sizeof(maximum.sun_path) + 1];
        char prefix[64];
        int prefix_len;
        size_t i;

        prefix_len = snprintf(prefix, sizeof(prefix),
                              "/tmp/aos-unix-max-%ld-", (long)getpid());
        memset(path, 'x', sizeof(maximum.sun_path));
        if (prefix_len > 0 && (size_t)prefix_len < sizeof(maximum.sun_path))
            memcpy(path, prefix, (size_t)prefix_len);
        path[sizeof(maximum.sun_path)] = '\0';
        unlink(path);

        memset(&maximum, 0, sizeof(maximum));
        maximum.sun_family = AF_UNIX;
        memcpy(maximum.sun_path, path, sizeof(maximum.sun_path));
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0 && bind(fd, (struct sockaddr *)&maximum,
                            path_offset + sizeof(maximum.sun_path)) == 0) {
            memset(&returned, 0xa5, sizeof(returned));
            returned_len = sizeof(returned);
            i = path_offset + sizeof(maximum.sun_path);
            contract_check("unix_max_pathname_length",
                getsockname(fd, (struct sockaddr *)&returned, &returned_len) == 0 &&
                returned_len == path_offset + sizeof(maximum.sun_path) + 1 &&
                memcmp((unsigned char *)&returned + path_offset, path,
                       sizeof(maximum.sun_path)) == 0 &&
                ((unsigned char *)&returned)[i] == '\0');
        } else {
            contract_check("unix_max_pathname_length", 0);
        }
        if (fd >= 0) close(fd);
        unlink(path);

        memset(&returned, 'y', sizeof(returned));
        ((struct sockaddr_un *)&returned)->sun_family = AF_UNIX;
        memcpy((unsigned char *)&returned + path_offset, path,
               sizeof(maximum.sun_path));
        ((unsigned char *)&returned)[path_offset + sizeof(maximum.sun_path)] = 'z';
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        errno = 0;
        contract_check("unix_oversize_path_einval",
            fd >= 0 && failed_with(
                bind(fd, (struct sockaddr *)&returned,
                     path_offset + sizeof(maximum.sun_path) + 1), EINVAL));
        if (fd >= 0) close(fd);
    }

    {
        struct sockaddr_un padded;
        char path[64];

        snprintf(path, sizeof(path), "/tmp/aos-unix-padded-%ld", (long)getpid());
        unlink(path);
        memset(&padded, 0, sizeof(padded));
        padded.sun_family = AF_UNIX;
        strncpy(padded.sun_path, path, sizeof(padded.sun_path) - 1);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        contract_check("unix_padded_struct_pathname",
            fd >= 0 && bind(fd, (struct sockaddr *)&padded, sizeof(padded)) == 0);
        if (fd >= 0) close(fd);
        unlink(path);
    }
}

static void name_pointer_contract(void) {
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    struct sockaddr_un listener_addr;
    socklen_t listener_len;
    int unconnected = socket(AF_UNIX, SOCK_STREAM, 0);
    int listener = -1;
    int client = -1;
    int accepted = -1;

    errno = 0;
    contract_check("getsockname_null_addr_efault",
        unconnected >= 0 && failed_with(getsockname(unconnected, NULL, &addrlen), EFAULT));
    errno = 0;
    contract_check("getsockname_null_len_efault",
        unconnected >= 0 && failed_with(
            getsockname(unconnected, (struct sockaddr *)&addr, NULL), EFAULT));
    errno = 0;
    contract_check("getsockname_null_both_efault",
        unconnected >= 0 && failed_with(getsockname(unconnected, NULL, NULL), EFAULT));
    errno = 0;
    contract_check("getsockname_badfd_precedes_pointer",
        failed_with(getsockname(-1, NULL, NULL), EBADF));
    errno = 0;
    contract_check("getpeername_unconnected_precedes_pointer",
        unconnected >= 0 && failed_with(getpeername(unconnected, NULL, NULL), ENOTCONN));
    errno = 0;
    contract_check("getpeername_badfd_precedes_pointer",
        failed_with(getpeername(-1, NULL, NULL), EBADF));

    listener = abstract_listener(10, 2, &listener_addr, &listener_len);
    if (listener >= 0) client = abstract_connect(&listener_addr, listener_len);
    if (client >= 0) accepted = accept(listener, NULL, NULL);
    addrlen = sizeof(addr);
    errno = 0;
    contract_check("getpeername_null_addr_efault",
        client >= 0 && accepted >= 0 &&
        failed_with(getpeername(client, NULL, &addrlen), EFAULT));
    errno = 0;
    contract_check("getpeername_null_len_efault",
        client >= 0 && accepted >= 0 && failed_with(
            getpeername(client, (struct sockaddr *)&addr, NULL), EFAULT));
    errno = 0;
    contract_check("getpeername_null_both_efault",
        client >= 0 && accepted >= 0 &&
        failed_with(getpeername(client, NULL, NULL), EFAULT));

    if (accepted >= 0) close(accepted);
    if (client >= 0) close(client);
    if (listener >= 0) close(listener);
    if (unconnected >= 0) close(unconnected);
}

static void accept_pointer_contract(void) {
    struct sockaddr_un listener_addr;
    struct sockaddr_storage peer;
    socklen_t listener_len;
    socklen_t peer_len = sizeof(peer);
    int listener = abstract_listener(20, 4, &listener_addr, &listener_len);
    int plain = socket(AF_UNIX, SOCK_STREAM, 0);
    int client1 = -1;
    int client2 = -1;
    int client3 = -1;
    int accepted1 = -1;
    int accepted2 = -1;
    int flags;

    errno = 0;
    contract_check("accept_badfd_precedes_pointer",
        failed_with(accept(-1, (struct sockaddr *)&peer, NULL), EBADF));
    errno = 0;
    contract_check("accept_nonlistener_precedes_pointer",
        plain >= 0 && failed_with(
            accept(plain, (struct sockaddr *)&peer, NULL), EINVAL));

    flags = listener >= 0 ? fcntl(listener, F_GETFL) : -1;
    if (flags >= 0) fcntl(listener, F_SETFL, flags | O_NONBLOCK);
    errno = 0;
    contract_check("accept_empty_nonblock_eagain",
        listener >= 0 && flags >= 0 &&
        failed_with(accept(listener, NULL, NULL), EAGAIN));

    if (listener >= 0) {
        client1 = abstract_connect(&listener_addr, listener_len);
        client2 = abstract_connect(&listener_addr, listener_len);
    }
    errno = 0;
    contract_check("accept_null_len_efault",
        client1 >= 0 && client2 >= 0 && failed_with(
            accept(listener, (struct sockaddr *)&peer, NULL), EFAULT));
    if (client1 >= 0 && client2 >= 0) {
        accepted1 = accept(listener, NULL, NULL);
        errno = 0;
        contract_check("accept_null_len_consumes_connection",
            accepted1 >= 0 && failed_with(accept(listener, NULL, NULL), EAGAIN));
        client3 = abstract_connect(&listener_addr, listener_len);
        peer_len = sizeof(peer);
        if (client3 >= 0) accepted2 = accept(listener, NULL, &peer_len);
    } else {
        contract_check("accept_null_len_consumes_connection", 0);
    }
    contract_check("accept_null_addr_null_len", accepted1 >= 0);
    contract_check("accept_null_addr_nonnull_len", accepted2 >= 0);

    if (accepted2 >= 0) close(accepted2);
    if (accepted1 >= 0) close(accepted1);
    if (client3 >= 0) close(client3);
    if (client2 >= 0) close(client2);
    if (client1 >= 0) close(client1);
    if (plain >= 0) close(plain);
    if (listener >= 0) close(listener);
}

static void lifecycle_contract(void) {
    struct sockaddr_un target_addr;
    struct sockaddr_un subject_addr;
    struct sockaddr_un second_addr;
    socklen_t target_len;
    socklen_t subject_len;
    socklen_t second_len;
    int target = abstract_listener(30, 4, &target_addr, &target_len);
    int subject = socket(AF_UNIX, SOCK_STREAM, 0);
    int client = -1;

    subject_len = unique_abstract_address(&subject_addr, 31);
    second_len = unique_abstract_address(&second_addr, 32);
    errno = 0;
    contract_check("unix_listen_before_bind_einval",
        subject >= 0 && failed_with(listen(subject, 1), EINVAL));
    if (subject >= 0 && subject_len > 0)
        contract_check("unix_first_bind",
            bind(subject, (struct sockaddr *)&subject_addr, subject_len) == 0);
    else
        contract_check("unix_first_bind", 0);
    errno = 0;
    contract_check("unix_second_bind_einval",
        subject >= 0 && second_len > 0 && failed_with(
            bind(subject, (struct sockaddr *)&second_addr, second_len), EINVAL));
    contract_check("unix_first_listen", subject >= 0 && listen(subject, 1) == 0);
    contract_check("unix_repeated_listen", subject >= 0 && listen(subject, 3) == 0);
    errno = 0;
    contract_check("unix_connect_after_listen_einval",
        subject >= 0 && target >= 0 && failed_with(
            connect(subject, (struct sockaddr *)&target_addr, target_len), EINVAL));

    if (target >= 0) client = abstract_connect(&target_addr, target_len);
    errno = 0;
    contract_check("unix_second_connect_eisconn",
        client >= 0 && failed_with(
            connect(client, (struct sockaddr *)&target_addr, target_len), EISCONN));

    if (client >= 0) close(client);
    if (subject >= 0) close(subject);
    if (target >= 0) close(target);
}

static int parity_contract(void) {
    contract_failures = 0;
    address_length_contract();
    name_pointer_contract();
    accept_pointer_contract();
    lifecycle_contract();
    printf("unix_socket_parity=%s\n", contract_failures == 0 ? "ok" : "FAIL");
    return contract_failures == 0 ? 0 : 1;
}

static int unsupported_type(int type) {
    int fd = socket(AF_UNIX, type, 0);
    if (fd >= 0) {
        close(fd);
        return 0;
    }
    return errno == EOPNOTSUPP
#if defined(ENOTSUP) && ENOTSUP != EOPNOTSUPP
        || errno == ENOTSUP
#endif
        ? 1 : -1;
}

static int socket_type_contract(void) {
    int dgram = unsupported_type(SOCK_DGRAM);
#ifdef SOCK_SEQPACKET
    int seqpacket = unsupported_type(SOCK_SEQPACKET);
#else
    int seqpacket = -2;
#endif
    printf("unix_dgram=%s\n",
           dgram == 0 ? "supported" : dgram == 1 ? "unsupported" : "wrong-errno");
    printf("unix_seqpacket=%s\n",
           seqpacket == 0 ? "supported" :
           seqpacket == 1 ? "unsupported" :
           seqpacket == -2 ? "not-defined" : "wrong-errno");
    return (dgram < 0 || seqpacket < 0) ? 1 : 0;
}

static socklen_t abstract_address(struct sockaddr_un *addr) {
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path + 1, abstract_name, sizeof(abstract_name) - 1);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
        sizeof(abstract_name));
}

static int abstract_client(void) {
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket(abstract client)");
        return 1;
    }
    socklen_t addrlen = abstract_address(&addr);
    if (connect(fd, (struct sockaddr *)&addr, addrlen) < 0) {
        perror("connect(abstract)");
        close(fd);
        return 1;
    }
    if (send(fd, "ping", 4, 0) != 4) {
        perror("send(abstract)");
        close(fd);
        return 1;
    }
    char reply[4];
    if (recv(fd, reply, sizeof(reply), MSG_WAITALL) != 4 ||
        memcmp(reply, "pong", 4) != 0) {
        fprintf(stderr, "abstract client received an invalid reply\n");
        close(fd);
        return 1;
    }
    /* Leave a real not-ready window between request frames. This catches a
     * stale POLLIN result after the first frame is consumed. */
    usleep(100 * 1000);
    if (send(fd, "next", 4, 0) != 4 ||
        recv(fd, reply, sizeof(reply), MSG_WAITALL) != 4 ||
        memcmp(reply, "done", 4) != 0) {
        fprintf(stderr, "abstract client second exchange failed\n");
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

static int abstract_namespace_contract(const char *program) {
    struct sockaddr_un addr;
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) {
        perror("socket(abstract listener)");
        return 1;
    }
    socklen_t addrlen = abstract_address(&addr);
    if (bind(listener, (struct sockaddr *)&addr, addrlen) < 0 ||
        listen(listener, 1) < 0) {
        perror("bind/listen(abstract)");
        close(listener);
        return 1;
    }

    pid_t child;
    char *child_argv[] = {(char *)program, (char *)"--abstract-client", NULL};
    int spawn_error = posix_spawnp(&child, program, NULL, NULL,
        child_argv, environ);
    if (spawn_error != 0) {
        fprintf(stderr, "posix_spawnp(abstract client): %s\n",
            strerror(spawn_error));
        close(listener);
        return 1;
    }

    int client = accept(listener, NULL, NULL);
    char request[4];
    struct pollfd ready = { .fd = client, .events = POLLIN };
    int flags = client >= 0 ? fcntl(client, F_GETFL) : -1;
    if (flags >= 0) fcntl(client, F_SETFL, flags | O_NONBLOCK);
    int ok = client >= 0 && flags >= 0 &&
        poll(&ready, 1, 1000) == 1 && (ready.revents & POLLIN) != 0 &&
        recv(client, request, sizeof(request), MSG_WAITALL) == 4 &&
        memcmp(request, "ping", 4) == 0 &&
        send(client, "pong", 4, 0) == 4;
    ready.revents = 0;
    ok = ok && poll(&ready, 1, 1000) == 1 &&
        (ready.revents & POLLIN) != 0 &&
        recv(client, request, sizeof(request), MSG_WAITALL) == 4 &&
        memcmp(request, "next", 4) == 0 &&
        send(client, "done", 4, 0) == 4;
    if (client >= 0) close(client);
    close(listener);

    int status = 0;
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        ok = 0;
    }
    printf("abstract_unix_namespace=%s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

#define STREAM_CONTRACT_BYTES (64 * 1024 + 37)
#define STREAM_CONTRACT_PEEK_BYTES 23

static socklen_t named_abstract_address(struct sockaddr_un *addr,
                                        const char *name) {
    size_t length = strlen(name);
    if (length + 1 > sizeof(addr->sun_path)) return 0;
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path + 1, name, length);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + length);
}

static unsigned char stream_contract_byte(size_t offset) {
    return (unsigned char)((offset * 31u + 7u) & 0xffu);
}

static int send_all_bytes(int fd, const unsigned char *bytes, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t sent = send(fd, bytes + offset, length - offset, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return -1;
        offset += (size_t)sent;
    }
    return 0;
}

static int stream_contract_client(const char *name) {
    struct sockaddr_un addr;
    socklen_t addrlen = named_abstract_address(&addr, name);
    unsigned char *payload = malloc(STREAM_CONTRACT_BYTES);
    unsigned char ack;
    int fd = -1;
    size_t i;
    int ok = 0;

    if (payload == NULL || addrlen == 0) {
        fprintf(stderr, "stream client setup failed\n");
        goto done;
    }
    for (i = 0; i < STREAM_CONTRACT_BYTES; i++)
        payload[i] = stream_contract_byte(i);
    fd = abstract_connect(&addr, addrlen);
    if (fd < 0) {
        perror("stream client connect");
        goto done;
    }
    if (send_all_bytes(fd, payload, STREAM_CONTRACT_PEEK_BYTES) < 0) {
        perror("stream client header send");
        goto done;
    }
    if (recv(fd, &ack, 1, 0) != 1 || ack != 0xa5) {
        perror("stream client ack recv");
        goto done;
    }
    if (send_all_bytes(fd, payload + STREAM_CONTRACT_PEEK_BYTES,
                       STREAM_CONTRACT_BYTES - STREAM_CONTRACT_PEEK_BYTES) < 0) {
        perror("stream client payload send");
        goto done;
    }
    usleep(150 * 1000);
    ok = send_all_bytes(fd, (const unsigned char *)"later", 5) == 0;

done:
    if (fd >= 0) close(fd);
    free(payload);
    return ok ? 0 : 1;
}

static int recv_pattern_odd_chunks(int fd, size_t start, size_t length) {
    static const size_t chunk_sizes[] = {1, 3, 7, 31, 257, 4093};
    unsigned char buffer[4093];
    size_t offset = 0;
    size_t turn = 0;

    while (offset < length) {
        size_t request = chunk_sizes[turn++ %
            (sizeof(chunk_sizes) / sizeof(chunk_sizes[0]))];
        size_t i;
        ssize_t received;
        if (request > length - offset) request = length - offset;
        received = recv(fd, buffer, request, 0);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return -1;
        for (i = 0; i < (size_t)received; i++) {
            if (buffer[i] != stream_contract_byte(start + offset + i))
                return -1;
        }
        offset += (size_t)received;
    }
    return 0;
}

static int stream_read_contract(const char *program) {
    struct sockaddr_un addr;
    char name[64];
    char *child_argv[] = {
        (char *)program, (char *)"--stream-contract-client", name, NULL
    };
    unsigned char first[STREAM_CONTRACT_PEEK_BYTES];
    unsigned char second[STREAM_CONTRACT_PEEK_BYTES];
    char delayed[5];
    socklen_t addrlen;
    pid_t child = -1;
    int listener = -1;
    int client = -1;
    int status = 0;
    int ok = 0;

    snprintf(name, sizeof(name), "aos-stream-%ld", (long)getpid());
    addrlen = named_abstract_address(&addr, name);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0 || addrlen == 0 ||
        bind(listener, (struct sockaddr *)&addr, addrlen) < 0 ||
        listen(listener, 1) < 0)
        goto done;
    if (posix_spawnp(&child, program, NULL, NULL, child_argv, environ) != 0)
        goto done;
    client = accept(listener, NULL, NULL);
    if (client < 0) {
        perror("stream server accept");
        goto done;
    }
    if (recv(client, first, sizeof(first), MSG_PEEK) != (ssize_t)sizeof(first)) {
        perror("stream server first peek");
        goto done;
    }
    if (recv(client, second, sizeof(second), MSG_PEEK) != (ssize_t)sizeof(second) ||
        memcmp(first, second, sizeof(first)) != 0) {
        perror("stream server repeated peek");
        goto done;
    }
    if (recv_pattern_odd_chunks(client, 0, STREAM_CONTRACT_PEEK_BYTES) < 0) {
        perror("stream server header drain");
        goto done;
    }
    if (send(client, "\xa5", 1, 0) != 1) {
        perror("stream server ack send");
        goto done;
    }
    if (recv_pattern_odd_chunks(
            client, STREAM_CONTRACT_PEEK_BYTES,
            STREAM_CONTRACT_BYTES - STREAM_CONTRACT_PEEK_BYTES) < 0) {
        perror("stream server payload drain");
        goto done;
    }
    if (recv(client, delayed, sizeof(delayed), 0) != (ssize_t)sizeof(delayed) ||
        memcmp(delayed, "later", sizeof(delayed)) != 0) {
        perror("stream server delayed recv");
        goto done;
    }
    ok = waitpid(child, &status, 0) == child &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0;
    child = -1;

done:
    if (client >= 0) close(client);
    if (listener >= 0) close(listener);
    if (child > 0) {
        waitpid(child, &status, 0);
    }
    printf("unix_stream_read_contract=%s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char *argv[]) {
    const char *path = "/tmp/test.sock";
    if (argc == 2 && strcmp(argv[1], "--abstract-client") == 0) {
        return abstract_client();
    }
    if (argc == 2 && strcmp(argv[1], "--abstract-contract") == 0) {
        return abstract_namespace_contract(argv[0]);
    }
    if (argc == 2 && strcmp(argv[1], "--parity-contract") == 0) {
        return parity_contract();
    }
    if (argc == 2 && strcmp(argv[1], "--socket-type-contract") == 0) {
        return socket_type_contract();
    }
    if (argc == 3 && strcmp(argv[1], "--stream-contract-client") == 0) {
        return stream_contract_client(argv[2]);
    }
    if (argc == 2 && strcmp(argv[1], "--stream-read-contract") == 0) {
        return stream_read_contract(argv[0]);
    }
    if (argc >= 2) {
        path = argv[1];
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    /* Linux accepts a full sockaddr_un and stops pathname parsing at the
     * first NUL; OpenSSH ControlMaster relies on this exact addrlen shape. */
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return 1;
    }

    if (listen(fd, 1) < 0) {
        perror("listen");
        close(fd);
        return 1;
    }

    printf("listening on %s\n", path);
    fflush(stdout);

    struct sockaddr_un client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
        perror("accept");
        close(fd);
        return 1;
    }

    char buf[256];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n < 0) {
        perror("recv");
        close(client_fd);
        close(fd);
        return 1;
    }
    buf[n] = '\0';

    printf("received: %s\n", buf);

    const char *reply = "pong";
    ssize_t sent = send(client_fd, reply, strlen(reply), 0);
    if (sent < 0) {
        perror("send");
        close(client_fd);
        close(fd);
        return 1;
    }

    printf("sent: %zd\n", sent);

    close(client_fd);
    close(fd);
    return 0;
}
