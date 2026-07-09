#define _GNU_SOURCE

/* exec_variants.c -- Linux parity for the less-common exec entry points. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int find_self(const char *argv0, char *out, size_t out_len) {
    if (strchr(argv0, '/')) {
        if (strlen(argv0) + 1 > out_len) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(out, argv0, strlen(argv0) + 1);
        return 0;
    }

    const char *path = getenv("PATH");
    if (!path) path = "/bin:/usr/bin";
    for (const char *segment = path;;) {
        const char *colon = strchr(segment, ':');
        size_t dir_len = colon ? (size_t)(colon - segment) : strlen(segment);
        int written = dir_len
            ? snprintf(out, out_len, "%.*s/%s", (int)dir_len, segment, argv0)
            : snprintf(out, out_len, "./%s", argv0);
        if (written >= 0 && (size_t)written < out_len && access(out, R_OK) == 0)
            return 0;
        if (!colon) break;
        segment = colon + 1;
    }
    errno = ENOENT;
    return -1;
}

static int copy_to_unlinked_fd(const char *source_path, int *out_fd) {
    int source = open(source_path, O_RDONLY);
    if (source < 0) return -1;

    char copy_path[] = "/tmp/agentos-fexecve-XXXXXX";
    int copy = mkstemp(copy_path);
    if (copy < 0) {
        close(source);
        return -1;
    }

    char buffer[8192];
    for (;;) {
        ssize_t length = read(source, buffer, sizeof(buffer));
        if (length < 0) goto fail;
        if (length == 0) break;
        for (ssize_t offset = 0; offset < length;) {
            ssize_t written = write(copy, buffer + offset, (size_t)(length - offset));
            if (written < 0) goto fail;
            offset += written;
        }
    }
    if (close(source) != 0) {
        source = -1;
        goto fail;
    }
    source = -1;
    if (fchmod(copy, 0700) != 0) {
        goto fail;
    }
    if (close(copy) != 0) {
        copy = -1;
        goto fail;
    }
    copy = open(copy_path, O_RDONLY);
    if (copy < 0) goto fail;
    if (unlink(copy_path) != 0) goto fail;
    if (fcntl(copy, F_SETFD, FD_CLOEXEC) != 0)
        goto fail;

    *out_fd = copy;
    return 0;

fail: {
        int saved_errno = errno;
        if (source >= 0) close(source);
        if (copy >= 0) close(copy);
        unlink(copy_path);
        errno = saved_errno;
        return -1;
    }
}

static int create_script(char *path_template, const char *contents) {
    int fd = mkstemp(path_template);
    if (fd < 0) return -1;
    size_t length = strlen(contents);
    for (size_t offset = 0; offset < length;) {
        ssize_t written = write(fd, contents + offset, length - offset);
        if (written < 0) {
            int saved_errno = errno;
            close(fd);
            unlink(path_template);
            errno = saved_errno;
            return -1;
        }
        offset += (size_t)written;
    }
    if (fchmod(fd, 0700) != 0 || close(fd) != 0) {
        int saved_errno = errno;
        unlink(path_template);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static int child_result(const char *name, int ok) {
    printf("%s: %s\n", name, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--execle-child") == 0) {
        int ok = argc == 3 && strcmp(argv[0], "execle-custom-argv0") == 0 &&
                 argv[2][0] == '\0' && getenv("EXECLE_VALUE") &&
                 strcmp(getenv("EXECLE_VALUE"), "ok") == 0;
        return child_result("execle", ok);
    }
    if (argc >= 2 && strcmp(argv[1], "--execvpe-child") == 0) {
        int ok = argc == 3 && strcmp(argv[0], "execvpe-custom-argv0") == 0 &&
                 argv[2][0] == '\0' && getenv("EXECVPE_VALUE") &&
                 strcmp(getenv("EXECVPE_VALUE"), "ok") == 0 && getenv("PATH") &&
                 strcmp(getenv("PATH"), "/definitely/not/searchable") == 0;
        return child_result("execvpe", ok);
    }
    if (argc >= 2 && strcmp(argv[1], "--fexecve-child") == 0) {
        int executable_fd = argc >= 3 ? atoi(argv[2]) : -1;
        errno = 0;
        int fd_closed = executable_fd >= 0 && fcntl(executable_fd, F_GETFD) == -1 &&
                        errno == EBADF;
        int ok = argc == 4 && strcmp(argv[0], "fexecve-custom-argv0") == 0 &&
                 argv[3][0] == '\0' && getenv("FEXECVE_VALUE") &&
                 strcmp(getenv("FEXECVE_VALUE"), "ok") == 0 && fd_closed;
        return child_result("fexecve_unlinked_cloexec", ok);
    }

    if (argc != 2) {
        fprintf(stderr,
                "usage: %s execle|execvpe|execve-shebang|shell-fallback|fexecve|"
                "fexecve-script|fexecve-script-cloexec\n",
                argv[0]);
        return 2;
    }

    char self_path[PATH_MAX];
    if (find_self(argv[0], self_path, sizeof(self_path)) != 0) {
        perror("find self");
        return 1;
    }

    if (strcmp(argv[1], "execle") == 0) {
        char *child_env[] = {"EXECLE_VALUE=ok", NULL};
        execle(self_path, "execle-custom-argv0", "--execle-child", "", NULL,
               child_env);
        perror("execle");
        return 1;
    }

    if (strcmp(argv[1], "execvpe") == 0) {
        char search_dir[PATH_MAX];
        if (strlen(self_path) + 1 > sizeof(search_dir)) {
            errno = ENAMETOOLONG;
            perror("execvpe path");
            return 1;
        }
        memcpy(search_dir, self_path, strlen(self_path) + 1);
        char *slash = strrchr(search_dir, '/');
        if (slash) *slash = '\0';
        else memcpy(search_dir, ".", 2);
        if (setenv("PATH", search_dir, 1) != 0) {
            perror("setenv PATH");
            return 1;
        }
        char *child_argv[] = {
            "execvpe-custom-argv0", "--execvpe-child", "", NULL,
        };
        char *child_env[] = {
            "EXECVPE_VALUE=ok", "PATH=/definitely/not/searchable", NULL,
        };
        const char *basename = strrchr(self_path, '/');
        execvpe(basename ? basename + 1 : self_path, child_argv, child_env);
        perror("execvpe");
        return 1;
    }

    if (strcmp(argv[1], "execve-shebang") == 0) {
        char script_path[] = "/tmp/agentos-exec-shebang-XXXXXX";
        const char contents[] =
            "#!/bin/sh\n"
            "case \"$0\" in /tmp/agentos-exec-shebang-*) ;; *) exit 8 ;; esac\n"
            "rm -f \"$0\"\n"
            "test \"$1\" = shebang-argument || exit 9\n"
            "printf 'execve_shebang: ok\\n'\n";
        if (create_script(script_path, contents) != 0) {
            perror("create shebang script");
            return 1;
        }
        char *child_argv[] = {"ignored-custom-script-argv0", "shebang-argument", NULL};
        extern char **environ;
        execve(script_path, child_argv, environ);
        unlink(script_path);
        perror("execve shebang");
        return 1;
    }

    if (strcmp(argv[1], "shell-fallback") == 0) {
        char script_path[] = "/tmp/agentos-exec-script-XXXXXX";
        int script = mkstemp(script_path);
        const char contents[] =
            "rm -f \"$0\"\n"
            "case \"$0\" in /tmp/agentos-exec-script-*) ;; *) exit 8 ;; esac\n"
            "test \"$1\" = shell-argument || exit 9\n"
            "printf 'shell_fallback: ok\\n'\n";
        if (script < 0 || write(script, contents, sizeof(contents) - 1) !=
                              (ssize_t)(sizeof(contents) - 1) ||
            fchmod(script, 0700) != 0 || close(script) != 0) {
            if (script >= 0) close(script);
            unlink(script_path);
            perror("create shell fallback script");
            return 1;
        }
        char search_path[PATH_MAX * 2];
        const char *original_path = getenv("PATH");
        int path_length = snprintf(search_path, sizeof(search_path), "/tmp:%s",
                                   original_path ? original_path : "/bin:/usr/bin");
        if (path_length < 0 || (size_t)path_length >= sizeof(search_path)) {
            unlink(script_path);
            errno = ENAMETOOLONG;
            perror("shell PATH");
            return 1;
        }
        char *basename = strrchr(script_path, '/');
        if (setenv("PATH", search_path, 1) != 0) {
            unlink(script_path);
            perror("setenv shell PATH");
            return 1;
        }
        if (!basename) {
            unlink(script_path);
            errno = EINVAL;
            perror("shell script path");
            return 1;
        }
        char *child_argv[] = {basename + 1, "shell-argument", NULL};
        execvp(basename + 1, child_argv);
        unlink(script_path);
        perror("execvp shell fallback");
        return 1;
    }

    if (strcmp(argv[1], "fexecve") == 0) {
        int executable_fd;
        if (copy_to_unlinked_fd(self_path, &executable_fd) != 0) {
            perror("prepare fexecve image");
            return 1;
        }
        char fd_arg[32];
        snprintf(fd_arg, sizeof(fd_arg), "%d", executable_fd);
        char *child_argv[] = {
            "fexecve-custom-argv0", "--fexecve-child", fd_arg, "", NULL,
        };
        char *child_env[] = {"FEXECVE_VALUE=ok", NULL};
        fexecve(executable_fd, child_argv, child_env);
        perror("fexecve");
        close(executable_fd);
        return 1;
    }

    if (strcmp(argv[1], "fexecve-script") == 0) {
        char script_path[] = "/tmp/agentos-fexec-script-XXXXXX";
        const char contents[] =
            "#!/bin/sh\n"
            "case \"$0\" in /dev/fd/*|/proc/self/fd/*) ;; *) exit 8 ;; esac\n"
            "test \"$1\" = fexecve-script-argument || exit 9\n"
            "printf 'fexecve_script_unlinked: ok\\n'\n";
        if (create_script(script_path, contents) != 0) {
            perror("create fexecve script");
            return 1;
        }
        int script_fd = open(script_path, O_RDONLY);
        if (script_fd < 0 || unlink(script_path) != 0) {
            if (script_fd >= 0) close(script_fd);
            unlink(script_path);
            perror("prepare unlinked fexecve script");
            return 1;
        }
        char *child_argv[] = {
            "ignored-fexecve-script-argv0", "fexecve-script-argument", NULL,
        };
        extern char **environ;
        fexecve(script_fd, child_argv, environ);
        perror("fexecve unlinked script");
        close(script_fd);
        return 1;
    }

    if (strcmp(argv[1], "fexecve-script-cloexec") == 0) {
        char script_path[] = "/tmp/agentos-fexec-cloexec-XXXXXX";
        const char contents[] = "#!/bin/sh\nprintf 'unexpected script execution\\n'\n";
        if (create_script(script_path, contents) != 0) {
            perror("create CLOEXEC fexecve script");
            return 1;
        }
        int script_fd = open(script_path, O_RDONLY);
        char renamed_path[PATH_MAX] = {0};
        int renamed_length = snprintf(renamed_path, sizeof(renamed_path), "%s.renamed",
                                      script_path);
        int prepare_errno = 0;
        if (script_fd < 0) prepare_errno = errno;
        else if (renamed_length < 0 ||
                 (size_t)renamed_length >= sizeof(renamed_path)) {
            prepare_errno = ENAMETOOLONG;
        } else if (rename(script_path, renamed_path) != 0) {
            prepare_errno = errno;
        } else if (fcntl(script_fd, F_SETFD, FD_CLOEXEC) != 0) {
            prepare_errno = errno;
        }
        if (prepare_errno != 0) {
            if (script_fd >= 0) close(script_fd);
            unlink(script_path);
            if (renamed_path[0] != '\0') unlink(renamed_path);
            errno = prepare_errno;
            perror("prepare CLOEXEC fexecve script");
            return 1;
        }
        char *child_argv[] = {"ignored-fexecve-cloexec-argv0", NULL};
        extern char **environ;
        errno = 0;
        int result = fexecve(script_fd, child_argv, environ);
        int exec_errno = errno;
        close(script_fd);
        unlink(renamed_path);
        int ok = result == -1 && exec_errno == ENOENT;
        return child_result("fexecve_script_cloexec_enoent", ok);
    }

    fprintf(stderr, "unknown exec variant: %s\n", argv[1]);
    return 2;
}
