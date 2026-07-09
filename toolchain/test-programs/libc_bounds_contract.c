#define _GNU_SOURCE

#include <errno.h>
#include <netdb.h>
#include <pwd.h>
#include <stdio.h>
#include <sys/resource.h>

int main(int argc, char **argv) {
    const char *host_name = argc > 1 ? argv[1] : "many.test";
    const char *service_name = argc > 2 ? argv[2] : "oversvc";
    const char *user_name = argc > 3 ? argv[3] : "oversuser";
    struct rlimit descriptors;
    struct hostent *host;
    struct servent *service;
    struct passwd *user;
    size_t address_count = 0;
    int service_error, passwd_error;

    if (getrlimit(RLIMIT_NOFILE, &descriptors) != 0) {
        perror("getrlimit(RLIMIT_NOFILE)");
        return 1;
    }
    printf("nofile_soft=%llu\n", (unsigned long long)descriptors.rlim_cur);
    printf("nofile_hard=%llu\n", (unsigned long long)descriptors.rlim_max);

    host = gethostbyname(host_name);
    if (host == NULL) {
        printf("host_addresses=error\n");
        printf("host_erange=%s\n", errno == ERANGE ? "yes" : "no");
        printf("host_no_recovery=%s\n",
            h_errno == NO_RECOVERY ? "yes" : "no");
    } else {
        while (host->h_addr_list[address_count] != NULL)
            address_count++;
        printf("host_addresses=%zu\n", address_count);
    }

    errno = 0;
    service = getservbyname(service_name, "tcp");
    service_error = errno;
    printf("service_found=%s\n", service != NULL ? "yes" : "no");
    printf("service_erange=%s\n", service_error == ERANGE ? "yes" : "no");

    errno = 0;
    user = getpwnam(user_name);
    passwd_error = errno;
    printf("passwd_found=%s\n", user != NULL ? "yes" : "no");
    printf("passwd_erange=%s\n", passwd_error == ERANGE ? "yes" : "no");
    return 0;
}
