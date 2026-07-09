#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _WASI_EMULATED_MMAN
#define _WASI_EMULATED_MMAN
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static size_t page_size(void) {
    long result = sysconf(_SC_PAGESIZE);
    return result > 0 ? (size_t)result : 65536u;
}

static int failed_with(int result, int expected_errno) {
    return result == -1 && errno == expected_errno;
}

struct tail_mapping {
    void *start;
    size_t length;
};

static int make_unmapped_tail(size_t page, struct tail_mapping *mapping) {
#ifdef __wasm__
    uintptr_t memory_end =
        (uintptr_t)__builtin_wasm_memory_size(0) * (uintptr_t)page;
    mapping->start = (void *)(memory_end - page);
    mapping->length = page * 2u;
    return 0;
#else
    void *start = mmap(NULL, page * 2u, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (start == MAP_FAILED) return -1;
    if (munmap((unsigned char *)start + page, page) != 0) {
        munmap(start, page * 2u);
        return -1;
    }
    mapping->start = start;
    mapping->length = page * 2u;
    return 0;
#endif
}

static void destroy_unmapped_tail(size_t page, struct tail_mapping mapping) {
#ifndef __wasm__
    (void)mapping.length;
    munmap(mapping.start, page);
#else
    (void)page;
    (void)mapping;
#endif
}

static int test_unmapped_tail(size_t page) {
    struct tail_mapping mapping;
    if (make_unmapped_tail(page, &mapping) != 0) return 10;
    errno = 0;
    int advice_ok =
        failed_with(madvise(mapping.start, mapping.length, MADV_NORMAL), ENOMEM);
    destroy_unmapped_tail(page, mapping);
    if (!advice_ok) return 11;
    puts("madvise_unmapped_tail_enomem=yes");
    return 0;
}

static int test_madvise(size_t page, unsigned char *memory) {
    memory[0] = 0x5a;
    if (madvise(memory, page, MADV_NORMAL) != 0 ||
        madvise(memory, page, MADV_RANDOM) != 0 ||
        madvise(memory, page, MADV_SEQUENTIAL) != 0 ||
        madvise(memory, page, MADV_WILLNEED) != 0 || memory[0] != 0x5a)
        return 20;
    if (madvise(memory, 0, MADV_NORMAL) != 0) return 21;

    errno = 0;
    if (!failed_with(madvise(memory + 1, page, MADV_NORMAL), EINVAL)) return 22;
    errno = 0;
    if (!failed_with(madvise(memory, page, 0x7fffffff), EINVAL)) return 23;

    void *overflow = (void *)(uintptr_t)(UINTPTR_MAX - page + 1u);
    errno = 0;
    if (!failed_with(madvise(overflow, page * 2u, MADV_NORMAL), EINVAL))
        return 24;
    puts("madvise_hints_and_validation=yes");
    return 0;
}

static int test_lock_validation(size_t page, unsigned char *memory) {
    if (mlock(memory, 0) != 0 || munlock(memory, 0) != 0) return 25;
    errno = 0;
    if (!failed_with(mlock((void *)(uintptr_t)UINTPTR_MAX, 0), EINVAL))
        return 26;
    errno = 0;
    if (!failed_with(munlock((void *)(uintptr_t)UINTPTR_MAX, 0), EINVAL))
        return 27;
    void *overflow = (void *)(uintptr_t)(UINTPTR_MAX - page + 1u);
    errno = 0;
    if (!failed_with(mlock(overflow, page * 2u), EINVAL)) return 28;
    errno = 0;
    if (!failed_with(munlock(overflow, page * 2u), EINVAL)) return 29;
    puts("memory_lock_range_validation=yes");
    return 0;
}

static int test_linux_specific(size_t page) {
#ifdef __wasm__
    (void)page;
    return 30;
#else
    unsigned char *memory = mmap(NULL, page, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return 31;
    if (mlock(memory, page) != 0 || munlock(memory, page) != 0) {
        munmap(memory, page);
        return 32;
    }
    puts("mlock_linux=yes");

    if (mlockall(MCL_FUTURE) != 0 || munlockall() != 0) {
        munmap(memory, page);
        return 33;
    }
    puts("mlockall_future_linux=yes");

    memory[0] = 0x5a;
    if (madvise(memory, page, MADV_DONTNEED) != 0 || memory[0] != 0) {
        munmap(memory, page);
        return 34;
    }
    munmap(memory, page);
    puts("madvise_dontneed_linux=yes");
    return 0;
#endif
}

static int test_wasm_specific(size_t page, unsigned char *memory) {
#ifndef __wasm__
    (void)page;
    (void)memory;
    return 40;
#else
    errno = 0;
    if (!failed_with(mlock(memory, page), ENOTSUP)) return 41;
    if (munlock(memory, page) != 0) return 42;
    if (mlock(memory, 0) != 0 || munlock(memory, 0) != 0) return 43;
    errno = 0;
    if (!failed_with(mlockall(MCL_CURRENT), ENOTSUP)) return 44;
    errno = 0;
    if (!failed_with(mlockall(MCL_FUTURE), ENOTSUP)) return 45;
    if (munlockall() != 0) return 46;
    errno = 0;
    if (!failed_with(mlockall(0), EINVAL)) return 47;
    puts("memory_locking_unsupported=yes");

    memory[0] = 0x5a;
    errno = 0;
    if (!failed_with(madvise(memory, page, MADV_DONTNEED), ENOTSUP) ||
        memory[0] != 0x5a)
        return 48;
    puts("madvise_dontneed_unsupported=yes");
    return 0;
#endif
}

int main(int argc, char **argv) {
    size_t page = page_size();
    unsigned char *memory = NULL;
    if (posix_memalign((void **)&memory, page, page * 2u) != 0) return 1;
    memset(memory, 0xa5, page * 2u);

    int result;
    if (argc == 2 && strcmp(argv[1], "--linux-specific") == 0)
        result = test_linux_specific(page);
    else if (argc == 2 && strcmp(argv[1], "--wasm-specific") == 0)
        result = test_wasm_specific(page, memory);
    else {
        result = test_unmapped_tail(page);
        if (result == 0) result = test_madvise(page, memory);
        if (result == 0) result = test_lock_validation(page, memory);
        if (result == 0) puts("madvise_contract=ok");
    }
    free(memory);
    return result;
}
