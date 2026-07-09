/* spawn_child.c -- posix_spawn 'echo hello', waitpid, print child stdout */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "posix_spawn_compat.h"

extern char **environ;

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--print-argv0") == 0) {
        printf("argv0=%s\n", argv[0]);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "--print-empty-args") == 0) {
        printf("empty_len=%zu tail=%s\n", strlen(argv[2]), argv[3]);
        return 0;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        return 1;
    }

    /* Redirect child stdout to pipe write end */
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);

    char *echo_argv[] = {"echo", "hello", NULL};
    pid_t child;
    int err = posix_spawnp(&child, "echo", &fa, NULL, echo_argv, environ);
    posix_spawn_file_actions_destroy(&fa);

    if (err != 0) {
        fprintf(stderr, "posix_spawn failed: %d\n", err);
        return 1;
    }

    close(pipefd[1]);

    char buf[256];
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);

    int status;
    waitpid(child, &status, 0);

    if (n > 0) {
        buf[n] = '\0';
        printf("child_stdout: %s", buf);
    } else {
        printf("child_stdout: (empty)\n");
    }
    printf("child_exit: %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    if (pipe(pipefd) != 0) {
        perror("pipe argv0");
        return 1;
    }
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);
    char *custom_argv[] = {"custom-argv0", "--print-argv0", NULL};
    err = posix_spawnp(&child, argv[0], &fa, NULL, custom_argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (err != 0) {
        fprintf(stderr, "posix_spawn argv0 failed: %d\n", err);
        return 1;
    }
    close(pipefd[1]);
    n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    waitpid(child, &status, 0);
    if (n <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("custom_argv0: failed\n");
        return 1;
    }
    buf[n] = '\0';
    printf("custom_%s", buf);

    if (pipe(pipefd) != 0) {
        perror("pipe empty argv");
        return 1;
    }
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);
    char *empty_argv[] = {"spawn-child", "--print-empty-args", "", "tail", NULL};
    err = posix_spawnp(&child, argv[0], &fa, NULL, empty_argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (err != 0) {
        fprintf(stderr, "posix_spawn empty argv failed: %d\n", err);
        return 1;
    }
    close(pipefd[1]);
    n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    waitpid(child, &status, 0);
    if (n <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("empty_argv: failed\n");
        return 1;
    }
    buf[n] = '\0';
    printf("%s", buf);

    // Unlike posix_spawnp(), posix_spawn() must not search PATH for a bare
    // executable name. Both native Linux and AgentOS should report ENOENT here.
    char *bare_argv[] = {"true", NULL};
    err = posix_spawn(&child, "true", NULL, NULL, bare_argv, environ);
    printf("spawn_bare_enoent: %s\n", err == ENOENT ? "yes" : "no");
    if (err == 0) {
        waitpid(child, &status, 0);
        return 1;
    }

    char *empty_path_argv[] = {"", NULL};
    err = posix_spawn(&child, "", NULL, NULL, empty_path_argv, environ);
    printf("spawn_empty_enoent: %s\n", err == ENOENT ? "yes" : "no");
    if (err == 0) {
        waitpid(child, &status, 0);
        return 1;
    }

    return 0;
}
