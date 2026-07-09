#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netdb.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

static int report(const char *name, int ok) {
    printf("%s=%s\n", name, ok ? "yes" : "no");
    return ok;
}

static int setrlimit_is_truthful(void) {
    struct rlimit before, requested, after;
    char byte;
    int blocked_error, blocked_fd, existing_fd, existing_ok, restored_ok;

    existing_fd = open("/etc/passwd", O_RDONLY);
    if (existing_fd < 0 || getrlimit(RLIMIT_NOFILE, &before) != 0 ||
        before.rlim_cur <= 3) {
        if (existing_fd >= 0)
            close(existing_fd);
        return 0;
    }
    requested = before;
    requested.rlim_cur = 3;
    if (setrlimit(RLIMIT_NOFILE, &requested) != 0 ||
        getrlimit(RLIMIT_NOFILE, &after) != 0 ||
        after.rlim_cur != requested.rlim_cur ||
        after.rlim_max != requested.rlim_max) {
        close(existing_fd);
        return 0;
    }

    existing_ok = read(existing_fd, &byte, 1) == 1;
    errno = 0;
    blocked_fd = open("/etc/passwd", O_RDONLY);
    blocked_error = errno;
    restored_ok = setrlimit(RLIMIT_NOFILE, &before) == 0;
    close(existing_fd);
    if (blocked_fd >= 0)
        close(blocked_fd);
    return existing_ok && blocked_fd == -1 && blocked_error == EMFILE &&
        restored_ok;
}

static int setrlimit_hard_raise_is_denied(void) {
    struct rlimit before, requested;

    if (getrlimit(RLIMIT_NOFILE, &before) != 0)
        return 0;
    if (before.rlim_max == RLIM_INFINITY)
        return 1;
    requested = before;
    requested.rlim_max++;
    errno = 0;
    return setrlimit(RLIMIT_NOFILE, &requested) == -1 && errno == EPERM;
}

int main(int argc, char **argv) {
    const char *host_name = argc > 1 ? argv[1] : "localhost";
    const char *service_name = argc > 2 ? argv[2] : "http";
    const char *user_name = argc > 3 ? argv[3] : "root";
    const char *group_name = argc > 4 ? argv[4] : "root";
    struct hostent *host;
    struct servent *service;
    struct passwd *user;
    struct group *group_by_name, *group_by_id, *entry;
    char address[INET_ADDRSTRLEN];
    char output[64];
    FILE *pipe;
    int status, found_group = 0, ok = 1;

    host = gethostbyname(host_name);
    ok &= report("host_lookup", host != NULL && host->h_addrtype == AF_INET &&
        host->h_length == (int)sizeof(struct in_addr) &&
        host->h_addr_list != NULL && host->h_addr_list[0] != NULL &&
        inet_ntop(AF_INET, host->h_addr_list[0], address, sizeof(address)) != NULL);

    service = getservbyname(service_name, "tcp");
    ok &= report("service_lookup", service != NULL && service->s_name != NULL &&
        service->s_proto != NULL && strcmp(service->s_proto, "tcp") == 0 &&
        ntohs((unsigned short)service->s_port) != 0);

    user = getpwnam(user_name);
    ok &= report("passwd_lookup", user != NULL &&
        strcmp(user->pw_name, user_name) == 0 && user->pw_dir != NULL &&
        user->pw_shell != NULL);

    group_by_name = getgrnam(group_name);
    group_by_id = group_by_name != NULL ? getgrgid(group_by_name->gr_gid) : NULL;
    ok &= report("group_lookup", group_by_name != NULL && group_by_id != NULL &&
        strcmp(group_by_id->gr_name, group_name) == 0);

    setgrent();
    while ((entry = getgrent()) != NULL) {
        if (strcmp(entry->gr_name, group_name) == 0) {
            found_group = 1;
            break;
        }
    }
    endgrent();
    ok &= report("group_enumeration", found_group);

    status = system(NULL) ? system("exit 7") : -1;
    ok &= report("system_shell", status >= 0 && WIFEXITED(status) &&
        WEXITSTATUS(status) == 7);

    pipe = popen("echo popen-read-ok", "r");
    output[0] = '\0';
    status = pipe != NULL && fgets(output, sizeof(output), pipe) != NULL
        ? pclose(pipe) : -1;
    ok &= report("popen_read", status >= 0 && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0 && strcmp(output, "popen-read-ok\n") == 0);

    pipe = popen("read line; test \"$line\" = popen-write-ok", "w");
    if (pipe != NULL)
        fputs("popen-write-ok\n", pipe);
    status = pipe != NULL ? pclose(pipe) : -1;
    ok &= report("popen_write", status >= 0 && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);

    pipe = popen("exit 0", "w");
    if (pipe != NULL) {
        fputs("buffered", pipe);
        close(fileno(pipe));
    }
    errno = 0;
    status = pipe != NULL ? pclose(pipe) : 0;
    ok &= report("pclose_close_error",
        pipe != NULL && status == -1 && errno == EBADF);

    ok &= report("setrlimit_truthful", setrlimit_is_truthful());
    ok &= report("setrlimit_hard_raise_denied",
        setrlimit_hard_raise_is_denied());

    errno = 0;
    pipe = popen("true", "x");
    ok &= report("popen_invalid_mode", pipe == NULL && errno == EINVAL);
    ok &= report("hstrerror_mapping",
        strstr(hstrerror(HOST_NOT_FOUND), "host") != NULL ||
        strstr(hstrerror(HOST_NOT_FOUND), "Host") != NULL);

    openlog("libc-compat", LOG_PID | LOG_PERROR, LOG_USER);
    syslog(LOG_WARNING, "syslog-visible=%d", 17);
    closelog();
    return ok ? 0 : 1;
}
