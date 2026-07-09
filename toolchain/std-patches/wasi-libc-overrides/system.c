#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

struct popen_child {
	FILE *stream;
	pid_t pid;
};

static struct popen_child *popen_children;
static size_t popen_children_capacity;
static int popen_limit_warning;

static void report_cleanup_error(const char *operation, int error) {
	fprintf(stderr, "%s: %s\n", operation, strerror(error));
}

static void remember_cleanup_error(const char *operation, int error,
	int *first_error) {
	if (*first_error == 0)
		*first_error = error;
	report_cleanup_error(operation, error);
}

static int initialize_popen_children(void) {
	struct rlimit descriptors;

	if (popen_children != NULL)
		return 0;
	if (getrlimit(RLIMIT_NOFILE, &descriptors) < 0)
		return -1;
	if (descriptors.rlim_cur == 0) {
		fprintf(stderr,
		    "popen: limits.resources.maxOpenFds is 0; raise limits.resources.maxOpenFds\n");
		errno = EMFILE;
		return -1;
	}
	if (descriptors.rlim_cur > SIZE_MAX / sizeof(*popen_children)) {
		errno = ENOMEM;
		return -1;
	}
	popen_children_capacity = (size_t)descriptors.rlim_cur;
	popen_children = calloc(popen_children_capacity, sizeof(*popen_children));
	if (popen_children == NULL) {
		popen_children_capacity = 0;
		return -1;
	}
	return 0;
}

static int wait_for_child(pid_t pid) {
	int status;

	while (waitpid(pid, &status, 0) < 0) {
		if (errno == EINTR)
			continue;
		return -1;
	}
	return status;
}

int system(const char *command) {
	struct sigaction ignore, old_int, old_quit;
	posix_spawnattr_t attributes;
	sigset_t block, old_mask, defaults;
	char *argv[] = {(char *)"sh", (char *)"-c", (char *)command, NULL};
	pid_t pid = -1;
	short spawn_flags = POSIX_SPAWN_SETSIGMASK;
	int error, saved_errno, status, cleanup_error = 0;
	int attributes_initialized = 0;

	if (command == NULL) {
		return access("/bin/sh", X_OK) == 0;
	}

	memset(&ignore, 0, sizeof(ignore));
	ignore.sa_handler = SIG_IGN;
	sigemptyset(&ignore.sa_mask);
	sigemptyset(&block);
	sigaddset(&block, SIGCHLD);
	if (sigaction(SIGINT, &ignore, &old_int) < 0)
		return -1;
	if (sigaction(SIGQUIT, &ignore, &old_quit) < 0) {
		saved_errno = errno;
		if (sigaction(SIGINT, &old_int, NULL) < 0)
			report_cleanup_error("system: restoring SIGINT", errno);
		errno = saved_errno;
		return -1;
	}
	if (sigprocmask(SIG_BLOCK, &block, &old_mask) < 0) {
		saved_errno = errno;
		if (sigaction(SIGQUIT, &old_quit, NULL) < 0)
			report_cleanup_error("system: restoring SIGQUIT", errno);
		if (sigaction(SIGINT, &old_int, NULL) < 0)
			report_cleanup_error("system: restoring SIGINT", errno);
		errno = saved_errno;
		return -1;
	}

	error = posix_spawnattr_init(&attributes);
	if (error == 0)
		attributes_initialized = 1;
	if (error == 0)
		error = posix_spawnattr_setsigmask(&attributes, &old_mask);
	sigemptyset(&defaults);
	if (old_int.sa_handler != SIG_IGN)
		sigaddset(&defaults, SIGINT);
	if (old_quit.sa_handler != SIG_IGN)
		sigaddset(&defaults, SIGQUIT);
	if (sigismember(&defaults, SIGINT) || sigismember(&defaults, SIGQUIT))
		spawn_flags |= POSIX_SPAWN_SETSIGDEF;
	if (error == 0 && (spawn_flags & POSIX_SPAWN_SETSIGDEF) != 0)
		error = posix_spawnattr_setsigdefault(&attributes, &defaults);
	if (error == 0)
		error = posix_spawnattr_setflags(&attributes, spawn_flags);
	if (error == 0)
		error = posix_spawn(&pid, "/bin/sh", NULL, &attributes, argv,
		    environ);
	if (attributes_initialized) {
		int destroy_error = posix_spawnattr_destroy(&attributes);
		if (destroy_error != 0)
			remember_cleanup_error("system: destroying spawn attributes",
			    destroy_error, &cleanup_error);
	}
	if (error != 0) {
		status = -1;
		errno = error;
	} else {
		status = wait_for_child(pid);
	}
	saved_errno = errno;
	if (sigprocmask(SIG_SETMASK, &old_mask, NULL) < 0)
		remember_cleanup_error("system: restoring signal mask", errno,
		    &cleanup_error);
	if (sigaction(SIGQUIT, &old_quit, NULL) < 0)
		remember_cleanup_error("system: restoring SIGQUIT", errno,
		    &cleanup_error);
	if (sigaction(SIGINT, &old_int, NULL) < 0)
		remember_cleanup_error("system: restoring SIGINT", errno,
		    &cleanup_error);
	if (status >= 0 && cleanup_error != 0) {
		errno = cleanup_error;
		return -1;
	}
	errno = saved_errno;
	return status;
}

FILE *popen(const char *command, const char *mode) {
	posix_spawn_file_actions_t actions;
	char *argv[] = {(char *)"sh", (char *)"-c", (char *)command, NULL};
	int pipe_fds[2], parent_fd, child_fd, child_target, error, flags;
	int read_mode, close_on_exec;
	int actions_initialized = 0;
	size_t slot = SIZE_MAX, open_count = 0;
	pid_t pid = -1;
	FILE *stream;

	if (command == NULL || mode == NULL ||
	    !((mode[0] == 'r' || mode[0] == 'w') &&
	    (mode[1] == '\0' || (mode[1] == 'e' && mode[2] == '\0')))) {
		errno = EINVAL;
		return NULL;
	}
	read_mode = mode[0] == 'r';
	close_on_exec = mode[1] == 'e';
	if (initialize_popen_children() < 0)
		return NULL;
	for (size_t i = 0; i < popen_children_capacity; i++) {
		if (popen_children[i].stream == NULL) {
			if (slot == SIZE_MAX)
				slot = i;
		} else {
			open_count++;
		}
	}
	if (slot == SIZE_MAX) {
		fprintf(stderr,
		    "popen: RLIMIT_NOFILE registry limit %zu reached; raise limits.resources.maxOpenFds\n",
		    popen_children_capacity);
		errno = EMFILE;
		return NULL;
	}
	if (open_count + 1 >=
	    popen_children_capacity - popen_children_capacity / 5) {
		if (!popen_limit_warning) {
			fprintf(stderr,
			    "popen: nearing RLIMIT_NOFILE registry limit (%zu/%zu open); raise limits.resources.maxOpenFds\n",
			    open_count + 1, popen_children_capacity);
			popen_limit_warning = 1;
		}
	} else {
		popen_limit_warning = 0;
	}
	if (pipe(pipe_fds) < 0)
		return NULL;
	parent_fd = read_mode ? pipe_fds[0] : pipe_fds[1];
	child_fd = read_mode ? pipe_fds[1] : pipe_fds[0];
	child_target = read_mode ? STDOUT_FILENO : STDIN_FILENO;

	error = posix_spawn_file_actions_init(&actions);
	if (error == 0)
		actions_initialized = 1;
	if (error == 0)
		error = posix_spawn_file_actions_addclose(&actions, parent_fd);
	if (error == 0)
		error = posix_spawn_file_actions_adddup2(&actions, child_fd,
		    child_target);
	if (error == 0 && child_fd != child_target)
		error = posix_spawn_file_actions_addclose(&actions, child_fd);
	for (size_t i = 0; error == 0 && i < popen_children_capacity; i++) {
		if (popen_children[i].stream == NULL)
			continue;
		error = posix_spawn_file_actions_addclose(&actions,
		    fileno(popen_children[i].stream));
	}
	if (error == 0)
		error = posix_spawn(&pid, "/bin/sh", &actions, NULL, argv, environ);
	if (actions_initialized) {
		int destroy_error = posix_spawn_file_actions_destroy(&actions);
		if (error == 0 && destroy_error != 0)
			error = destroy_error;
		else if (destroy_error != 0)
			report_cleanup_error("popen: destroying file actions",
			    destroy_error);
	}
	if (error != 0) {
		if (close(pipe_fds[0]) < 0)
			report_cleanup_error("popen: closing read pipe", errno);
		if (close(pipe_fds[1]) < 0)
			report_cleanup_error("popen: closing write pipe", errno);
		if (pid > 0) {
			if (kill(pid, SIGKILL) < 0)
				report_cleanup_error("popen: killing child after setup failure",
				    errno);
			if (wait_for_child(pid) < 0)
				report_cleanup_error("popen: reaping child after setup failure",
				    errno);
		}
		errno = error;
		return NULL;
	}
	if (close(child_fd) < 0) {
		int close_error = errno;
		if (close(parent_fd) < 0)
			report_cleanup_error("popen: closing parent pipe after setup failure",
			    errno);
		if (kill(pid, SIGKILL) < 0)
			report_cleanup_error("popen: killing child after close failure",
			    errno);
		if (wait_for_child(pid) < 0)
			report_cleanup_error("popen: reaping child after close failure",
			    errno);
		errno = close_error;
		return NULL;
	}
	if (close_on_exec) {
		flags = fcntl(parent_fd, F_GETFD);
		if (flags < 0 || fcntl(parent_fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
			int saved_errno = errno;
			if (close(parent_fd) < 0)
				report_cleanup_error("popen: closing parent pipe", errno);
			if (kill(pid, SIGKILL) < 0)
				report_cleanup_error("popen: killing child after fcntl failure",
				    errno);
			if (wait_for_child(pid) < 0)
				report_cleanup_error("popen: reaping child after fcntl failure",
				    errno);
			errno = saved_errno;
			return NULL;
		}
	}
	stream = fdopen(parent_fd, read_mode ? "r" : "w");
	if (stream == NULL) {
		int saved_errno = errno;
		if (close(parent_fd) < 0)
			report_cleanup_error("popen: closing parent pipe after fdopen failure",
			    errno);
		if (kill(pid, SIGKILL) < 0)
			report_cleanup_error("popen: killing child after fdopen failure",
			    errno);
		if (wait_for_child(pid) < 0)
			report_cleanup_error("popen: reaping child after fdopen failure",
			    errno);
		errno = saved_errno;
		return NULL;
	}
	popen_children[slot].stream = stream;
	popen_children[slot].pid = pid;
	return stream;
}

int pclose(FILE *stream) {
	pid_t pid = -1;
	int close_error, close_errno = 0, status;

	if (stream == NULL) {
		errno = EINVAL;
		return -1;
	}
	for (size_t i = 0; i < popen_children_capacity; i++) {
		if (popen_children[i].stream == stream) {
			pid = popen_children[i].pid;
			popen_children[i].stream = NULL;
			popen_children[i].pid = 0;
			break;
		}
	}
	if (pid < 0) {
		errno = ECHILD;
		return -1;
	}
	close_error = fclose(stream);
	if (close_error != 0)
		close_errno = errno;
	status = wait_for_child(pid);
	if (close_error != 0) {
		errno = close_errno;
		return -1;
	}
	if (status < 0)
		return -1;
	return status;
}
