/* userinfo.c — print identity and unprivileged group-change behavior */
#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

int main(void) {
    printf("uid=%u\n", (unsigned)getuid());
    printf("gid=%u\n", (unsigned)getgid());
    printf("euid=%u\n", (unsigned)geteuid());
    printf("egid=%u\n", (unsigned)getegid());
    errno = 0;
    int setgroups_result = setgroups(0, NULL);
    int setgroups_blocked = setgroups_result == -1 && errno == EPERM;
    printf("setgroups_unprivileged_eperm=%s\n",
        setgroups_blocked ? "yes" : "no");
    return setgroups_blocked ? 0 : 1;
}
