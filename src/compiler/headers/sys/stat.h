#ifndef _RATCC_SYS_STAT_H
#define _RATCC_SYS_STAT_H

#if defined(_WIN32)
#error "<sys/stat.h> is not available when targeting windows"
#endif

#include <sys/types.h>

#define __NEED_struct_timespec
#include <bits/alltypes.h>

/* glibc x86-64 struct stat; the padding and field order are load bearing */
struct stat {
	dev_t st_dev;
	ino_t st_ino;
	nlink_t st_nlink;
	mode_t st_mode;
	uid_t st_uid;
	gid_t st_gid;
	unsigned int __rat_pad0;
	dev_t st_rdev;
	off_t st_size;
	blksize_t st_blksize;
	blkcnt_t st_blocks;
	struct timespec st_atim;
	struct timespec st_mtim;
	struct timespec st_ctim;
	long __rat_reserved[3];
};

#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec

#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define S_IFREG 0100000
#define S_IFIFO 0010000
#define S_IFLNK 0120000
#define S_IFSOCK 0140000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 070
#define S_IRGRP 040
#define S_IWGRP 020
#define S_IXGRP 010
#define S_IRWXO 07
#define S_IROTH 04
#define S_IWOTH 02
#define S_IXOTH 01
#define S_IREAD S_IRUSR
#define S_IWRITE S_IWUSR
#define S_IEXEC S_IXUSR

int stat(const char* path, struct stat* out);
int lstat(const char* path, struct stat* out);
int fstat(int fd, struct stat* out);
int fstatat(int dirfd, const char* path, struct stat* out, int flags);
int chmod(const char* path, mode_t mode);
int fchmod(int fd, mode_t mode);
int mkdir(const char* path, mode_t mode);
int mkfifo(const char* path, mode_t mode);
int mknod(const char* path, mode_t mode, dev_t dev);
mode_t umask(mode_t mask);

#endif
