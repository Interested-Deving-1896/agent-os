#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
    struct addrinfo hints, *addresses = NULL, *address;
    int connected = 0, error;

    if (argc != 3) return 2;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    error = getaddrinfo(argv[1], argv[2], &hints, &addresses);
    if (error != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(error));
        return 1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        int fd = socket(address->ai_family, address->ai_socktype,
            address->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0)
            connected = 1;
        close(fd);
        if (connected) break;
    }
    freeaddrinfo(addresses);
    printf("getaddrinfo_live_hosts_service_connect=%s\n",
        connected ? "yes" : "no");
    return connected ? 0 : 1;
}
