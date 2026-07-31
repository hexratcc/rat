#ifndef _RATCC_FCNTL_H
#define _RATCC_FCNTL_H

#if defined(_WIN32)
#error "<fcntl.h> is not available when targeting windows"
#endif

#include <sys/types.h>

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NONBLOCK 04000

int open(const char* path, int flags, ...);
int creat(const char* path, mode_t mode);

#endif
