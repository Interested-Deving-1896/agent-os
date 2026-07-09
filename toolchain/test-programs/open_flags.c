#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char directory[] = "/tmp/agentos-open-flags-XXXXXX";
    if (mkdtemp(directory) == NULL)
        return 1;

    char target[512];
    char link[512];
    char missing[512];
    char umask_target[512];
    if (snprintf(target, sizeof(target), "%s/target", directory) >= (int)sizeof(target) ||
        snprintf(link, sizeof(link), "%s/link", directory) >= (int)sizeof(link) ||
        snprintf(missing, sizeof(missing), "%s/missing", directory) >= (int)sizeof(missing) ||
        snprintf(umask_target, sizeof(umask_target), "%s/umask", directory) >=
            (int)sizeof(umask_target))
        return 1;

    int fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, "payload", 7) != 7 || close(fd) != 0 ||
        symlink(target, link) != 0)
        return 1;

    errno = 0;
    fd = open(target, O_RDONLY | O_DIRECTORY | O_TRUNC);
    int directory_truncate_errno = errno;
    if (fd >= 0)
        close(fd);

    errno = 0;
    fd = open(link, O_RDONLY | O_NOFOLLOW | O_TRUNC);
    int nofollow_truncate_errno = errno;
    if (fd >= 0)
        close(fd);

    errno = 0;
    fd = open(missing, O_RDONLY | O_DIRECTORY | O_CREAT, 0600);
    int directory_create_errno = errno;
    if (fd >= 0)
        close(fd);

    char contents[8] = {0};
    fd = open(target, O_RDONLY);
    int content_intact = fd >= 0 && read(fd, contents, 7) == 7 &&
        memcmp(contents, "payload", 7) == 0;
    if (fd >= 0)
        close(fd);
    int missing_absent = access(missing, F_OK) == -1 && errno == ENOENT;

    mode_t previous_umask = umask(0077);
    fd = open(umask_target, O_WRONLY | O_CREAT | O_EXCL, 0666);
    struct stat umask_stat;
    int umask_applied = fd >= 0 && close(fd) == 0 &&
        stat(umask_target, &umask_stat) == 0 &&
        (umask_stat.st_mode & 0777) == 0600;
    umask(previous_umask);

    int ok = directory_truncate_errno == ENOTDIR &&
        nofollow_truncate_errno == ELOOP && directory_create_errno == EINVAL &&
        content_intact && missing_absent && umask_applied;
    printf("open_directory_truncate_enotdir=%s\n",
        directory_truncate_errno == ENOTDIR ? "yes" : "no");
    printf("open_nofollow_truncate_eloop=%s\n",
        nofollow_truncate_errno == ELOOP ? "yes" : "no");
    printf("open_directory_create_einval=%s\n",
        directory_create_errno == EINVAL ? "yes" : "no");
    printf("open_failure_side_effects_absent=%s\n",
        content_intact && missing_absent ? "yes" : "no");
    printf("umask_applies_to_kernel_create=%s\n", umask_applied ? "yes" : "no");
    printf("open_flags=%s\n", ok ? "ok" : "failed");

    unlink(umask_target);
    unlink(missing);
    unlink(link);
    unlink(target);
    rmdir(directory);
    return ok ? 0 : 1;
}
