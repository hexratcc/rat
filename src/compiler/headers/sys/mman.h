#ifndef _RATCC_SYS_MMAN_H
#define _RATCC_SYS_MMAN_H

#if defined(_WIN32)
#error "<sys/mman.h> is not available when targeting windows"
#endif

#include <sys/types.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4

#define MAP_FILE 0
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_GROWSDOWN 0x0100
#define MAP_DENYWRITE 0x0800
#define MAP_EXECUTABLE 0x1000
#define MAP_LOCKED 0x2000
#define MAP_NORESERVE 0x4000
#define MAP_POPULATE 0x8000
#define MAP_NONBLOCK 0x10000
#define MAP_STACK 0x20000

#define MAP_FAILED ((void*)-1)

#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2

#define MS_ASYNC 1
#define MS_INVALIDATE 2
#define MS_SYNC 4

#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4

#define POSIX_MADV_NORMAL 0
#define POSIX_MADV_RANDOM 1
#define POSIX_MADV_SEQUENTIAL 2
#define POSIX_MADV_WILLNEED 3
#define POSIX_MADV_DONTNEED 4

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void* addr, size_t length);
int mprotect(void* addr, size_t length, int prot);
void* mremap(void* addr, size_t oldLength, size_t newLength, int flags, ...);
int msync(void* addr, size_t length, int flags);
int madvise(void* addr, size_t length, int advice);
int posix_madvise(void* addr, size_t length, int advice);

#endif
