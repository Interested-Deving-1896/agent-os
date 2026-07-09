/**
 * POSIX ownership and umask surface for AgentOS WASI.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wasi/api.h>
#include <wasi/libc-find-relpath.h>

#define AGENTOS_HIDDEN_PREOPEN_FD_TAG 0x40000000
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

__attribute__((import_module("host_process"), import_name("proc_umask")))
__wasi_errno_t host_proc_umask(uint32_t mask, uint32_t *ret_previous);

__attribute__((import_module("host_fs"), import_name("chown")))
__wasi_errno_t host_fs_chown(uint32_t fd, const char *path, size_t path_len,
                            uint32_t uid, uint32_t gid,
                            uint32_t follow_symlinks);

__attribute__((import_module("host_fs"), import_name("fchown")))
__wasi_errno_t host_fs_fchown(uint32_t fd, uint32_t uid, uint32_t gid);

int __wasilibc_find_relpath_fallback(const char *path, const char **abs_prefix,
                                    char **relative_buf,
                                    size_t *relative_buf_len,
                                    int can_realloc);

static int ownership_result(__wasi_errno_t error) {
	if (error == __WASI_ERRNO_SUCCESS)
		return 0;
	errno = error;
	return -1;
}

static int resolve_ordinary_path(const char *path, char **relative) {
	static __thread char *relative_buf;
	static __thread size_t relative_buf_len;
	const char *abs_prefix;
	int fd;
	if (__wasilibc_find_relpath_alloc) {
		fd = __wasilibc_find_relpath_alloc(path, &abs_prefix, &relative_buf,
		                                  &relative_buf_len, 1);
	} else {
		fd = __wasilibc_find_relpath_fallback(path, &abs_prefix, &relative_buf,
		                                     &relative_buf_len, 1);
	}
	if (fd < 0)
		return -1;
	*relative = relative_buf;
	return fd | AGENTOS_HIDDEN_PREOPEN_FD_TAG;
}

static int ownership_path_at(int fd, const char *path, uid_t owner, gid_t group,
                             int follow_symlinks) {
	char *relative = (char *)path;
	if (fd == AT_FDCWD || path[0] == '/') {
		fd = resolve_ordinary_path(path, &relative);
		if (fd < 0)
			return -1;
	}
	return ownership_result(host_fs_chown(
	    (uint32_t)fd, relative, strlen(relative), (uint32_t)owner,
	    (uint32_t)group, follow_symlinks ? 1 : 0));
}

mode_t umask(mode_t mask) {
	uint32_t previous = 0;
	__wasi_errno_t error = host_proc_umask((uint32_t)mask & 0777, &previous);
	if (error != __WASI_ERRNO_SUCCESS) {
		errno = error;
		return (mode_t)-1;
	}
	return (mode_t)previous;
}

int chown(const char *path, uid_t owner, gid_t group) {
	return ownership_path_at(AT_FDCWD, path, owner, group, 1);
}

int fchown(int fd, uid_t owner, gid_t group) {
	return ownership_result(
	    host_fs_fchown((uint32_t)fd, (uint32_t)owner, (uint32_t)group));
}

int lchown(const char *path, uid_t owner, gid_t group) {
	return ownership_path_at(AT_FDCWD, path, owner, group, 0);
}

int fchownat(int fd, const char *path, uid_t owner, gid_t group, int flags) {
	if ((flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) != 0) {
		errno = EINVAL;
		return -1;
	}
	if (path[0] == '\0' && (flags & AT_EMPTY_PATH) != 0) {
		if (fd == AT_FDCWD)
			return ownership_path_at(fd, ".", owner, group, 1);
		return fchown(fd, owner, group);
	}
	if (path[0] == '\0') {
		errno = ENOENT;
		return -1;
	}
	return ownership_path_at(fd, path, owner, group,
	                         (flags & AT_SYMLINK_NOFOLLOW) == 0);
}
