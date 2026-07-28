#ifndef _RATCC_UNISTD_H
#define _RATCC_UNISTD_H

#if defined(_WIN32)
#error "<unistd.h> is not available when targeting windows"
#endif

#include <sys/types.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int unlink(const char* path);
pid_t getpid(void);
unsigned sleep(unsigned seconds);
int usleep(unsigned useconds);

#endif
