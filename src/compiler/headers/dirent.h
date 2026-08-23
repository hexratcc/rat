#ifndef _RATCC_DIRENT_H
#define _RATCC_DIRENT_H

#if defined(_WIN32)
#error "<dirent.h> is not available when targeting windows"
#endif

#include <sys/types.h>

struct dirent {
	ino_t d_ino;
	off_t d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[256];
};

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14

typedef struct __rat_DIR DIR;

DIR* opendir(const char* path);
DIR* fdopendir(int fd);
int closedir(DIR* dir);
struct dirent* readdir(DIR* dir);
void rewinddir(DIR* dir);
void seekdir(DIR* dir, long loc);
long telldir(DIR* dir);
int dirfd(DIR* dir);

#endif
