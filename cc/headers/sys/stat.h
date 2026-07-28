#ifndef _RATCC_SYS_STAT_H
#define _RATCC_SYS_STAT_H

#if defined(_WIN32)
#error "<sys/stat.h> is not available when targeting windows"
#endif

#include <sys/types.h>

struct stat {
	dev_t st_dev;
	ino_t st_ino;
	nlink_t st_nlink;
	unsigned int st_mode;
	uid_t st_uid;
	gid_t st_gid;
	unsigned int __rat_pad0;
	dev_t st_rdev;
	long st_size;
	blksize_t st_blksize;
	blkcnt_t st_blocks;
	time_t st_atime;
	long st_atime_nsec;
	time_t st_mtime;
	long st_mtime_nsec;
	time_t st_ctime;
	long st_ctime_nsec;
	long __rat_reserved[3];
};

#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define S_IFREG 0100000
#define S_IFIFO 0010000
#define S_IFLNK 0120000
#define S_IFSOCK 0140000
#define S_ISDIR(m) (((m)&S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m)&S_IFMT) == S_IFREG)

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

int stat(const char* path, struct stat* out);
int fstat(int fd, struct stat* out);
int chmod(const char* path, mode_t mode);
int mkdir(const char* path, mode_t mode);

#endif
