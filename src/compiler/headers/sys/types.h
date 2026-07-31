#ifndef _RATCC_SYS_TYPES_H
#define _RATCC_SYS_TYPES_H

#if defined(_WIN32)
#error "<sys/types.h> is not available when targeting windows"
#endif

#define __NEED_size_t
#define __NEED_ssize_t
#define __NEED_off_t
#define __NEED_time_t
#include <bits/alltypes.h>

typedef int pid_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int mode_t;
typedef unsigned long ino_t;
typedef unsigned long dev_t;
typedef unsigned long nlink_t;
typedef long blksize_t;
typedef long blkcnt_t;

#endif
