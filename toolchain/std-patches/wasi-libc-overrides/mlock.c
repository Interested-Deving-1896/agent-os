/**
 * Memory residency compatibility for AgentOS WASM guests.
 *
 * V8 owns the WebAssembly linear-memory backing store. AgentOS cannot pin
 * those host pages today, so the mlock family must fail explicitly instead
 * of reporting a security property that was not applied. Access-pattern
 * madvise calls are advisory and may be ignored, as on Linux; operations that
 * would change mappings, contents, fork, or core-dump behavior are unsupported.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _WASI_EMULATED_MMAN
#define _WASI_EMULATED_MMAN
#endif
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>

#include <__macro_PAGESIZE.h>

#define MAX_LINEAR_PAGES 65536u

struct page_range {
	uint32_t first;
	uint32_t end;
};

static uint64_t linear_memory_bytes(void) {
	return (uint64_t)__builtin_wasm_memory_size(0) * (uint64_t)PAGESIZE;
}

static int checked_page_range(const void *addr, size_t len,
		int require_page_alignment, struct page_range *range) {
	uint64_t start = (uintptr_t)addr;
	if (require_page_alignment && (start & (PAGESIZE - 1u)) != 0) {
		errno = EINVAL;
		return -1;
	}

	const uint64_t address_space_bytes = (uint64_t)UINTPTR_MAX + 1u;
	if ((uint64_t)len > address_space_bytes - start) {
		errno = EINVAL;
		return -1;
	}
	uint64_t end = start + (uint64_t)len;
	uint64_t rounded_end = end / PAGESIZE;
	if ((end & (PAGESIZE - 1u)) != 0) rounded_end++;
	uint64_t first = start / PAGESIZE;
	if (first > MAX_LINEAR_PAGES || rounded_end > MAX_LINEAR_PAGES ||
			(len == 0 && rounded_end == MAX_LINEAR_PAGES &&
			 rounded_end > first)) {
		errno = EINVAL;
		return -1;
	}
	if (end > linear_memory_bytes() ||
			rounded_end > __builtin_wasm_memory_size(0)) {
		errno = ENOMEM;
		return -1;
	}
	range->first = (uint32_t)first;
	range->end = (uint32_t)rounded_end;
	return 0;
}

static int unsupported_lock_range(const void *addr, size_t len) {
	struct page_range range;
	if (checked_page_range(addr, len, 0, &range) != 0) return -1;
	if (range.first == range.end) return 0;
	errno = ENOTSUP;
	return -1;
}

int mlock(const void *addr, size_t len) {
	return unsupported_lock_range(addr, len);
}

int munlock(const void *addr, size_t len) {
	struct page_range range;
	if (checked_page_range(addr, len, 0, &range) != 0) return -1;
	/* No guest lock can be established, so the requested range is unlocked. */
	return 0;
}

int mlockall(int flags) {
	const int known = MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT;
	if (flags == 0 || (flags & ~known) != 0 ||
			((flags & MCL_ONFAULT) != 0 &&
			 (flags & (MCL_CURRENT | MCL_FUTURE)) == 0)) {
		errno = EINVAL;
		return -1;
	}
	errno = ENOTSUP;
	return -1;
}

int munlockall(void) {
	/* No guest lock can be established, so all guest memory is unlocked. */
	return 0;
}

static int known_unsupported_advice(int advice) {
	switch (advice) {
	case MADV_DONTNEED:
	case MADV_FREE:
	case MADV_REMOVE:
	case MADV_DONTFORK:
	case MADV_DOFORK:
	case MADV_MERGEABLE:
	case MADV_UNMERGEABLE:
	case MADV_HUGEPAGE:
	case MADV_NOHUGEPAGE:
	case MADV_DONTDUMP:
	case MADV_DODUMP:
	case MADV_WIPEONFORK:
	case MADV_KEEPONFORK:
	case MADV_COLD:
	case MADV_PAGEOUT:
	case MADV_HWPOISON:
	case MADV_SOFT_OFFLINE:
		return 1;
	default:
		return 0;
	}
}

int madvise(void *addr, size_t len, int advice) {
	struct page_range range;
	if (checked_page_range(addr, len, 1, &range) != 0) return -1;
	(void)range;
	switch (advice) {
	case MADV_NORMAL:
	case MADV_RANDOM:
	case MADV_SEQUENTIAL:
	case MADV_WILLNEED:
		/* Linux treats these as best-effort hints, so ignoring them is valid. */
		return 0;
	default:
		if (known_unsupported_advice(advice)) errno = ENOTSUP;
		else errno = EINVAL;
		return -1;
	}
}
