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

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#define _SC_ARG_MAX 0
#define _SC_CHILD_MAX 1
#define _SC_CLK_TCK 2
#define _SC_NGROUPS_MAX 3
#define _SC_OPEN_MAX 4
#define _SC_STREAM_MAX 5
#define _SC_TZNAME_MAX 6
#define _SC_JOB_CONTROL 7
#define _SC_SAVED_IDS 8
#define _SC_VERSION 29
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE 30
#define _SC_LINE_MAX 43
#define _SC_IOV_MAX 60
#define _SC_GETGR_R_SIZE_MAX 69
#define _SC_GETPW_R_SIZE_MAX 70
#define _SC_LOGIN_NAME_MAX 71
#define _SC_TTY_NAME_MAX 72
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PHYS_PAGES 85
#define _SC_AVPHYS_PAGES 86
#define _SC_ATEXIT_MAX 87
#define _SC_NZERO 109
#define _SC_SYMLOOP_MAX 173
#define _SC_HOST_NAME_MAX 180

#define _PC_LINK_MAX 0
#define _PC_NAME_MAX 3
#define _PC_PATH_MAX 4
#define _PC_PIPE_BUF 5

ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
ssize_t pread(int fd, void* buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int access(const char* path, int mode);
int fsync(int fd);
int fdatasync(int fd);
int truncate(const char* path, off_t length);
int ftruncate(int fd, off_t length);
int chown(const char* path, uid_t owner, gid_t group);
int fchown(int fd, uid_t owner, gid_t group);
int lchown(const char* path, uid_t owner, gid_t group);
int chdir(const char* path);
int fchdir(int fd);
char* getcwd(char* buf, size_t size);
int rmdir(const char* path);
int unlink(const char* path);
int link(const char* oldpath, const char* newpath);
int symlink(const char* target, const char* path);
ssize_t readlink(const char* path, char* buf, size_t size);
int dup(int fd);
int dup2(int oldfd, int newfd);
int pipe(int fds[2]);
int isatty(int fd);
char* ttyname(int fd);
long sysconf(int name);
long fpathconf(int fd, int name);
long pathconf(const char* path, int name);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
pid_t getpid(void);
pid_t getppid(void);
pid_t fork(void);
int execv(const char* path, char* const argv[]);
int execvp(const char* file, char* const argv[]);
unsigned alarm(unsigned seconds);
unsigned sleep(unsigned seconds);
int usleep(useconds_t useconds);
int gethostname(char* name, size_t size);
void _exit(int status);

#endif
