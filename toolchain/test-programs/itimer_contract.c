#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static volatile sig_atomic_t alarm_calls;

static void alarm_handler(int signal) {
    (void)signal;
    alarm_calls++;
}

static long long timeval_us(const struct timeval *value) {
    return (long long)value->tv_sec * 1000000 + value->tv_usec;
}

static int test_real_remaining(void) {
    struct itimerval configured;
    struct itimerval queried;
    struct itimerval previous;
    struct itimerval disabled;
    memset(&configured, 0, sizeof(configured));
    memset(&disabled, 0, sizeof(disabled));
    configured.it_value.tv_sec = 2;
    configured.it_interval.tv_usec = 750000;
    if (setitimer(ITIMER_REAL, &configured, NULL) != 0) return 0;
    usleep(50000);
    if (getitimer(ITIMER_REAL, &queried) != 0 ||
        setitimer(ITIMER_REAL, &disabled, &previous) != 0)
        return 0;
    long long queried_remaining = timeval_us(&queried.it_value);
    long long previous_remaining = timeval_us(&previous.it_value);
    int remaining = queried_remaining > 0 && queried_remaining <= 2000000 &&
        previous_remaining > 0 && previous_remaining <= queried_remaining;
    int interval = timeval_us(&queried.it_interval) == 750000 &&
        timeval_us(&previous.it_interval) == 750000;
    if (getitimer(ITIMER_REAL, &queried) != 0)
        return 0;
    int cancelled = timeval_us(&queried.it_value) == 0 &&
        timeval_us(&queried.it_interval) == 0;
    return remaining && interval && cancelled;
}

static int test_alarm_rounding(void) {
    if (alarm(2) != 0)
        return 0;
    usleep(50000);
    return alarm(0) == 2;
}

static int test_masked_repeating(void) {
    struct sigaction action;
    struct itimerval repeating;
    struct itimerval disabled;
    sigset_t alarm_set;
    memset(&action, 0, sizeof(action));
    memset(&repeating, 0, sizeof(repeating));
    memset(&disabled, 0, sizeof(disabled));
    action.sa_handler = alarm_handler;
    sigemptyset(&action.sa_mask);
    sigemptyset(&alarm_set);
    sigaddset(&alarm_set, SIGALRM);
    repeating.it_value.tv_usec = 20000;
    repeating.it_interval.tv_usec = 20000;
    alarm_calls = 0;
    if (sigaction(SIGALRM, &action, NULL) != 0 ||
        sigprocmask(SIG_BLOCK, &alarm_set, NULL) != 0 ||
        setitimer(ITIMER_REAL, &repeating, NULL) != 0)
        return 0;
    usleep(120000);
    int delayed = alarm_calls == 0;
    if (setitimer(ITIMER_REAL, &disabled, NULL) != 0 ||
        sigprocmask(SIG_UNBLOCK, &alarm_set, NULL) != 0)
        return 0;
    usleep(10000);
    int coalesced = delayed && alarm_calls == 1;
    if (!coalesced)
        fprintf(stderr, "itimer masked: delayed=%d alarm_calls=%d\n",
            delayed, (int)alarm_calls);
    return coalesced;
}

static int test_timer_selectors(void) {
    struct itimerval value;
    struct itimerval disabled;
    memset(&disabled, 0, sizeof(disabled));
    errno = 0;
    int invalid = getitimer(-1, &value) == -1 && errno == EINVAL;
    int virtual_ok = getitimer(ITIMER_VIRTUAL, &value) == 0 &&
        timeval_us(&value.it_value) == 0 &&
        timeval_us(&value.it_interval) == 0 &&
        setitimer(ITIMER_VIRTUAL, &disabled, NULL) == 0;
    int prof_ok = getitimer(ITIMER_PROF, &value) == 0 &&
        timeval_us(&value.it_value) == 0 &&
        timeval_us(&value.it_interval) == 0 &&
        setitimer(ITIMER_PROF, &disabled, NULL) == 0;
    return invalid && virtual_ok && prof_ok;
}

static int test_default_termination(const char *self) {
    pid_t child = -1;
    char *child_argv[] = {(char *)self, "--default-alarm", NULL};
    int error = posix_spawnp(&child, self, NULL, NULL, child_argv, environ);
    int status = 0;
    pid_t waited = error == 0 ? waitpid(child, &status, 0) : -1;
    int terminated = error == 0 && waited == child &&
        WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM;
    if (!terminated)
        fprintf(stderr,
            "itimer default: spawn=%d child=%d waited=%d status=%d exited=%d exit=%d signaled=%d signal=%d\n",
            error, (int)child, (int)waited, status, WIFEXITED(status),
            WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status),
            WIFSIGNALED(status) ? WTERMSIG(status) : -1);
    return terminated;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--default-alarm") == 0) {
        alarm(1);
        sleep(3);
        return 90;
    }

    int real = test_real_remaining();
    int rounding = test_alarm_rounding();
    int masked = test_masked_repeating();
    int selectors = test_timer_selectors();
    int termination = test_default_termination(argv[0]);
    printf("itimer_real_remaining=%s\n", real ? "yes" : "no");
    printf("alarm_rounding=%s\n", rounding ? "yes" : "no");
    printf("itimer_masked_coalesced=%s\n", masked ? "yes" : "no");
    printf("itimer_selector_policy=%s\n", selectors ? "yes" : "no");
    printf("alarm_default_termination=%s\n", termination ? "yes" : "no");
    int ok = real && rounding && masked && selectors && termination;
    printf("itimer_contract=%s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}
