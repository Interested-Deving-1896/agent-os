#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

static int fail(const char *label) {
    fprintf(stderr, "%s: %s\n", label, strerror(errno));
    return 1;
}

int main(void) {
    char target[128];
    char link_path[128];
    char alias_path[128];
    char directory_path[128];
    snprintf(target, sizeof(target), "chown-contract-%ld", (long)getpid());
    snprintf(link_path, sizeof(link_path), "chown-contract-link-%ld", (long)getpid());
    snprintf(alias_path, sizeof(alias_path), "chown-contract-alias-%ld", (long)getpid());
    snprintf(directory_path, sizeof(directory_path), "chown-contract-dir-%ld", (long)getpid());
    rmdir(directory_path);
    unlink(alias_path);
    unlink(link_path);
    unlink(target);

    int fd = open(target, O_CREAT | O_EXCL | O_RDWR, 06755);
    if (fd < 0)
        return fail("open target");
    if (fchmod(fd, 06755) != 0)
        return fail("fchmod target");
    if (symlink(target, link_path) != 0)
        return fail("symlink");

    struct stat before;
    if (fstat(fd, &before) != 0)
        return fail("initial fstat");
    if (lchown(link_path, (uid_t)-1, (gid_t)-1) != 0)
        return fail("lchown");
    struct stat after_lchown;
    if (fstat(fd, &after_lchown) != 0)
        return fail("fstat after lchown");
    int lchown_preserved_target = (after_lchown.st_mode & 06000) == 06000;

    if (chown(link_path, (uid_t)-1, (gid_t)-1) != 0)
        return fail("chown");
    struct stat after_chown;
    if (fstat(fd, &after_chown) != 0)
        return fail("fstat after chown");
    int chown_followed_target = (after_chown.st_mode & 06000) == 0;
    int ids_preserved = before.st_uid == after_chown.st_uid &&
                        before.st_gid == after_chown.st_gid;

    if (fchmod(fd, 06745) != 0)
        return fail("set non-executable setgid mode");
    if (chown(target, (uid_t)-1, (gid_t)-1) != 0)
        return fail("chown non-executable setgid file");
    struct stat after_nonexec_chown;
    if (fstat(fd, &after_nonexec_chown) != 0)
        return fail("fstat non-executable setgid file");
    int nonexec_setgid_preserved =
        (after_nonexec_chown.st_mode & 06777) == 02745;

    errno = 0;
    int uid_change = fchown(fd, before.st_uid + 1, (gid_t)-1);
    int foreign_uid_eperm = uid_change == -1 && errno == EPERM;

    if (link(target, alias_path) != 0)
        return fail("hard link target");
    if (fchmod(fd, 06745) != 0)
        return fail("reset mode");
    if (unlink(target) != 0)
        return fail("unlink open target");
    if (fchownat(fd, "", (uid_t)-1, (gid_t)-1, AT_EMPTY_PATH) != 0)
        return fail("fchownat empty path");
    struct stat after_empty_path;
    if (fstat(fd, &after_empty_path) != 0)
        return fail("fstat empty path");
    struct stat after_alias;
    if (stat(alias_path, &after_alias) != 0)
        return fail("stat hard-link alias");
    int empty_path_used_fd =
        after_empty_path.st_ino == after_alias.st_ino &&
        (after_empty_path.st_mode & 06777) == 02745 &&
        (after_alias.st_mode & 06777) == 02745;

    if (mkdir(directory_path, 0777) != 0)
        return fail("mkdir detached directory");
    int directory_fd = open(directory_path, O_RDONLY | O_DIRECTORY);
    if (directory_fd < 0)
        return fail("open detached directory");
    if (rmdir(directory_path) != 0 || fchmod(directory_fd, 0701) != 0)
        return fail("fchmod detached directory");
    struct stat directory_stat;
    if (fstat(directory_fd, &directory_stat) != 0)
        return fail("fstat detached directory");
    int detached_directory_fchmod =
        S_ISDIR(directory_stat.st_mode) && (directory_stat.st_mode & 0777) == 0701;
    close(directory_fd);

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0 || fchmod(pipe_fds[0], 0640) != 0)
        return fail("fchmod pipe");
    struct stat pipe_read_stat;
    struct stat pipe_write_stat;
    if (fstat(pipe_fds[0], &pipe_read_stat) != 0 ||
        fstat(pipe_fds[1], &pipe_write_stat) != 0)
        return fail("fstat pipe");
    int pipe_fchmod = S_ISFIFO(pipe_read_stat.st_mode) &&
        (pipe_read_stat.st_mode & 0777) == 0640 &&
        pipe_read_stat.st_ino == pipe_write_stat.st_ino &&
        (pipe_write_stat.st_mode & 0777) == 0640;
    close(pipe_fds[0]);
    close(pipe_fds[1]);

    int socket_fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, socket_fds) != 0 ||
        fchmod(socket_fds[0], 0601) != 0)
        return fail("fchmod socket");
    struct stat socket_stat;
    if (fstat(socket_fds[0], &socket_stat) != 0)
        return fail("fstat socket");
    int socket_fchmod =
        S_ISSOCK(socket_stat.st_mode) && (socket_stat.st_mode & 0777) == 0601;
    close(socket_fds[0]);
    close(socket_fds[1]);

    errno = 0;
    int invalid = fchownat(AT_FDCWD, link_path, (uid_t)-1, (gid_t)-1, 0x2000);
    int invalid_flags_einval = invalid == -1 && errno == EINVAL;

    close(fd);
    unlink(alias_path);
    unlink(link_path);
    printf("lchown_preserved_target=%s\n", lchown_preserved_target ? "yes" : "no");
    printf("chown_followed_target=%s\n", chown_followed_target ? "yes" : "no");
    printf("unchanged_ids_preserved=%s\n", ids_preserved ? "yes" : "no");
    printf("nonexec_setgid_preserved=%s\n", nonexec_setgid_preserved ? "yes" : "no");
    printf("foreign_uid_eperm=%s\n", foreign_uid_eperm ? "yes" : "no");
    printf("fchownat_empty_path=%s\n", empty_path_used_fd ? "yes" : "no");
    printf("detached_directory_fchmod=%s\n", detached_directory_fchmod ? "yes" : "no");
    printf("pipe_fchmod=%s\n", pipe_fchmod ? "yes" : "no");
    printf("socket_fchmod=%s\n", socket_fchmod ? "yes" : "no");
    printf("fchownat_invalid_flags=%s\n", invalid_flags_einval ? "yes" : "no");
    return lchown_preserved_target && chown_followed_target && ids_preserved &&
                   nonexec_setgid_preserved && foreign_uid_eperm &&
                   empty_path_used_fd && detached_directory_fchmod &&
                   pipe_fchmod && socket_fchmod && invalid_flags_einval
               ? 0
               : 1;
}
