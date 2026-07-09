#define _GNU_SOURCE

/* waitpid_edge.c -- Linux wait selector and state-transition parity tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "posix_spawn_compat.h"

extern char **environ;

static volatile sig_atomic_t sigchld_count = 0;

static void count_sigchld(int signal) {
    (void)signal;
    sigchld_count++;
}

static pid_t waitpid_retry(pid_t pid, int *status, int options) {
    pid_t result;
    do {
        result = waitpid(pid, status, options);
    } while (result == -1 && errno == EINTR);
    return result;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--slow-exit") == 0) {
        usleep(250000);
        return 11;
    }
    if (argc == 2 && strcmp(argv[1], "--fast-exit") == 0) {
        return 22;
    }
    if (argc == 2 && strcmp(argv[1], "--delayed-fast-exit") == 0) {
        usleep(250000);
        return 23;
    }
    if (argc == 2 && strcmp(argv[1], "--pause-loop") == 0) {
        for (;;) usleep(100000);
    }
    /* Test 1: spawn 3 children with different exit codes, waitpid each by specific PID */
    {
        pid_t c1, c2, c3;
        char *a1[] = {"sh", "-c", "exit 1", NULL};
        char *a2[] = {"sh", "-c", "exit 2", NULL};
        char *a3[] = {"sh", "-c", "exit 3", NULL};
        int err;

        err = posix_spawnp(&c1, "sh", NULL, NULL, a1, environ);
        if (err != 0) { printf("test1: FAIL (spawn c1 err=%d)\n", err); return 1; }

        err = posix_spawnp(&c2, "sh", NULL, NULL, a2, environ);
        if (err != 0) { printf("test1: FAIL (spawn c2 err=%d)\n", err); return 1; }

        err = posix_spawnp(&c3, "sh", NULL, NULL, a3, environ);
        if (err != 0) { printf("test1: FAIL (spawn c3 err=%d)\n", err); return 1; }

        int s1, s2, s3;
        pid_t r1 = waitpid(c1, &s1, 0);
        pid_t r2 = waitpid(c2, &s2, 0);
        pid_t r3 = waitpid(c3, &s3, 0);

        int e1 = WIFEXITED(s1) ? WEXITSTATUS(s1) : -1;
        int e2 = WIFEXITED(s2) ? WEXITSTATUS(s2) : -1;
        int e3 = WIFEXITED(s3) ? WEXITSTATUS(s3) : -1;

        int ok = (r1 == c1 && e1 == 1 &&
                  r2 == c2 && e2 == 2 &&
                  r3 == c3 && e3 == 3);
        printf("test1_c1_exit: %d\n", e1);
        printf("test1_c2_exit: %d\n", e2);
        printf("test1_c3_exit: %d\n", e3);
        printf("test1: %s\n", ok ? "ok" : "FAIL");
    }

    /* Test 2: spawn 2 children, use wait() (waitpid -1) twice, verify both reaped */
    {
        pid_t c1, c2;
        char *a1[] = {"true", NULL};
        char *a2[] = {"true", NULL};
        int err;

        err = posix_spawnp(&c1, "true", NULL, NULL, a1, environ);
        if (err != 0) { printf("test2: FAIL (spawn c1 err=%d)\n", err); return 1; }

        err = posix_spawnp(&c2, "true", NULL, NULL, a2, environ);
        if (err != 0) { printf("test2: FAIL (spawn c2 err=%d)\n", err); return 1; }

        int s1, s2;
        pid_t r1 = wait(&s1);
        pid_t r2 = wait(&s2);

        /* Both returned PIDs must be valid (> 0) and distinct */
        int valid = (r1 > 0 && r2 > 0 && r1 != r2);
        /* Both must be one of c1 or c2 */
        int known = ((r1 == c1 || r1 == c2) && (r2 == c1 || r2 == c2));
        /* Both must have exited successfully */
        int exited = (WIFEXITED(s1) && WEXITSTATUS(s1) == 0 &&
                      WIFEXITED(s2) && WEXITSTATUS(s2) == 0);

        printf("test2_r1_valid: %s\n", (r1 > 0) ? "yes" : "no");
        printf("test2_r2_valid: %s\n", (r2 > 0) ? "yes" : "no");
        printf("test2_distinct: %s\n", (r1 != r2) ? "yes" : "no");
        printf("test2: %s\n", (valid && known && exited) ? "ok" : "FAIL");
    }

    /* Test 3: waitpid with PID that was never spawned, verify returns -1 */
    {
        errno = 0;
        int status;
        pid_t ret = waitpid(99999, &status, 0);
        int err = errno;
        int failed = (ret == -1 && err == ECHILD);
        printf("test3_ret: %d\n", (int)ret);
        printf("test3_errno: %d\n", err);
        printf("test3_failed: %s\n", failed ? "yes" : "no");
        printf("test3: %s\n", failed ? "ok" : "FAIL");
    }

    /* Test 4: a normal exit(137) is not a SIGKILL termination. */
    {
        pid_t child;
        char *argv[] = {"sh", "-c", "exit 137", NULL};
        int err = posix_spawnp(&child, "sh", NULL, NULL, argv, environ);
        if (err != 0) { printf("test4: FAIL (spawn err=%d)\n", err); return 1; }

        int status;
        pid_t ret = waitpid(child, &status, 0);
        int ok = ret == child && WIFEXITED(status) &&
                 WEXITSTATUS(status) == 137 && !WIFSIGNALED(status);
        printf("test4_exited: %s\n", WIFEXITED(status) ? "yes" : "no");
        printf("test4_exit: %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        printf("test4_signaled: %s\n", WIFSIGNALED(status) ? "yes" : "no");
        printf("test4: %s\n", ok ? "ok" : "FAIL");
    }

    /* Test 5: waitpid(-1) returns a ready child, not the first child spawned. */
    {
        pid_t slow, fast;
        char *slow_argv[] = {"waitpid-edge-slow", "--slow-exit", NULL};
        char *fast_argv[] = {"waitpid-edge-fast", "--fast-exit", NULL};
        int err = posix_spawnp(&slow, argv[0], NULL, NULL, slow_argv, environ);
        if (err != 0) { printf("test5: FAIL (spawn slow err=%d)\n", err); return 1; }
        err = posix_spawnp(&fast, argv[0], NULL, NULL, fast_argv, environ);
        if (err != 0) { printf("test5: FAIL (spawn fast err=%d)\n", err); return 1; }

        int status;
        pid_t ret = waitpid(-1, &status, 0);
        int ok = ret == fast && WIFEXITED(status) && WEXITSTATUS(status) == 22;
        printf("test5_first_ready: %s\n", ok ? "yes" : "no");
        printf("test5: %s\n", ok ? "ok" : "FAIL");

        if (ret != slow) {
            waitpid(slow, NULL, 0);
        }
        if (ret != fast) {
            waitpid(fast, NULL, 0);
        }
    }

    /* Test 6: pid=0 selects only children in the caller's process group. */
    {
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setpgroup(&attr, 0);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        pid_t foreign, local;
        char *fast_argv[] = {"waitpid-edge-foreign", "--fast-exit", NULL};
        char *slow_argv[] = {"waitpid-edge-local", "--slow-exit", NULL};
        int err = posix_spawnp(&foreign, argv[0], NULL, &attr,
            fast_argv, environ);
        posix_spawnattr_destroy(&attr);
        if (err != 0) { printf("test6: FAIL (spawn foreign err=%d)\n", err); return 1; }
        err = posix_spawnp(&local, argv[0], NULL, NULL, slow_argv, environ);
        if (err != 0) { printf("test6: FAIL (spawn local err=%d)\n", err); return 1; }
        int status = 0;
        pid_t ret = waitpid(0, &status, 0);
        int ok = ret == local && WIFEXITED(status) && WEXITSTATUS(status) == 11;
        printf("test6_pid_zero_group: %s\n", ok ? "yes" : "no");
        printf("test6: %s\n", ok ? "ok" : "FAIL");
        waitpid(foreign, NULL, 0);
    }

    /* Test 7: pid<-1 selects children in the named process group. */
    {
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setpgroup(&attr, 0);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        pid_t child;
        char *slow_argv[] = {"waitpid-edge-negative-group", "--slow-exit", NULL};
        int err = posix_spawnp(&child, argv[0], NULL, &attr,
            slow_argv, environ);
        posix_spawnattr_destroy(&attr);
        if (err != 0) { printf("test7: FAIL (spawn err=%d)\n", err); return 1; }
        int status = 0;
        pid_t ret = waitpid(-child, &status, 0);
        int ok = ret == child && WIFEXITED(status) && WEXITSTATUS(status) == 11;
        printf("test7_negative_group: %s\n", ok ? "yes" : "no");
        printf("test7: %s\n", ok ? "ok" : "FAIL");
    }

    /* Test 8: stopped and continued states use Linux raw wait statuses. */
    {
        pid_t child;
        char *pause_argv[] = {"waitpid-edge-paused", "--pause-loop", NULL};
        int err = posix_spawnp(&child, argv[0], NULL, NULL,
            pause_argv, environ);
        if (err != 0) { printf("test8: FAIL (spawn err=%d)\n", err); return 1; }
        int stopped_status = 0;
        int continued_status = 0;
        int terminated_status = 0;
        int stop_sent = kill(child, SIGSTOP) == 0;
        pid_t stopped = waitpid(child, &stopped_status, WUNTRACED);
        int stopped_ok = stopped == child && WIFSTOPPED(stopped_status) &&
            WSTOPSIG(stopped_status) == SIGSTOP;
        int continue_sent = kill(child, SIGCONT) == 0;
        pid_t continued = waitpid(child, &continued_status, WCONTINUED);
        int continued_ok = continued == child && WIFCONTINUED(continued_status);
        int term_sent = kill(child, SIGTERM) == 0;
        pid_t terminated = waitpid(child, &terminated_status, 0);
        int terminated_ok = terminated == child && WIFSIGNALED(terminated_status) &&
            WTERMSIG(terminated_status) == SIGTERM;
        int ok = stop_sent && stopped_ok && continue_sent && continued_ok &&
            term_sent && terminated_ok;
        printf("test8_stopped: %s\n", stopped_ok ? "yes" : "no");
        printf("test8_continued: %s\n", continued_ok ? "yes" : "no");
        printf("test8_terminated: %s\n", terminated_ok ? "yes" : "no");
        printf("test8: %s\n", ok ? "ok" : "FAIL");
    }

    /* Test 9: unknown option bits fail without consuming the child. */
    {
        pid_t child;
        char *slow_argv[] = {"waitpid-edge-options", "--slow-exit", NULL};
        int err = posix_spawnp(&child, argv[0], NULL, NULL,
            slow_argv, environ);
        if (err != 0) { printf("test9: FAIL (spawn err=%d)\n", err); return 1; }
        errno = 0;
        int status = 0;
        pid_t ret = waitpid(child, &status, 0x10000000);
        int invalid = ret == -1 && errno == EINVAL;
        pid_t reaped = waitpid(child, &status, 0);
        int preserved = reaped == child && WIFEXITED(status) &&
            WEXITSTATUS(status) == 11;
        printf("test9_invalid_options: %s\n", invalid ? "yes" : "no");
        printf("test9_child_preserved: %s\n", preserved ? "yes" : "no");
        printf("test9: %s\n", invalid && preserved ? "ok" : "FAIL");
    }

    /* Test 10: SIGCHLD from a non-selected child interrupts blocking waitpid. */
    {
        struct sigaction action;
        struct sigaction previous_action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = count_sigchld;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGCHLD, &action, &previous_action) != 0) {
            printf("test10: FAIL (sigaction errno=%d)\n", errno);
            return 1;
        }

        pid_t selected;
        pid_t interrupter;
        char *selected_argv[] = {"waitpid-edge-selected", "--pause-loop", NULL};
        char *interrupter_argv[] = {
            "waitpid-edge-interrupter", "--delayed-fast-exit", NULL
        };
        int err = posix_spawnp(&selected, argv[0], NULL, NULL,
            selected_argv, environ);
        if (err != 0) {
            printf("test10: FAIL (spawn selected err=%d)\n", err);
            sigaction(SIGCHLD, &previous_action, NULL);
            return 1;
        }
        err = posix_spawnp(&interrupter, argv[0], NULL, NULL,
            interrupter_argv, environ);
        if (err != 0) {
            printf("test10: FAIL (spawn interrupter err=%d)\n", err);
            kill(selected, SIGKILL);
            waitpid_retry(selected, NULL, 0);
            sigaction(SIGCHLD, &previous_action, NULL);
            return 1;
        }

        sigchld_count = 0;
        errno = 0;
        int selected_status = 0;
        pid_t selected_result = waitpid(selected, &selected_status, 0);
        int selected_errno = errno;
        int interrupted = selected_result == -1 && selected_errno == EINTR;
        int handled = sigchld_count > 0;

        int interrupter_status = 0;
        pid_t reaped_interrupter = waitpid_retry(interrupter,
            &interrupter_status, 0);
        int interrupter_ok = reaped_interrupter == interrupter &&
            WIFEXITED(interrupter_status) && WEXITSTATUS(interrupter_status) == 23;

        int killed = kill(selected, SIGKILL) == 0;
        int selected_cleanup_status = 0;
        pid_t reaped_selected = waitpid_retry(selected,
            &selected_cleanup_status, 0);
        int cleanup_ok = killed && reaped_selected == selected &&
            WIFSIGNALED(selected_cleanup_status) &&
            WTERMSIG(selected_cleanup_status) == SIGKILL;
        sigaction(SIGCHLD, &previous_action, NULL);

        printf("test10_sigchld_handler: %s\n", handled ? "yes" : "no");
        printf("test10_waitpid_eintr: %s\n", interrupted ? "yes" : "no");
        printf("test10_children_cleaned: %s\n",
            interrupter_ok && cleanup_ok ? "yes" : "no");
        printf("test10: %s\n",
            handled && interrupted && interrupter_ok && cleanup_ok ? "ok" : "FAIL");
    }

    return 0;
}
