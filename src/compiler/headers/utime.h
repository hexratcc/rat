#ifndef _RATCC_UTIME_H
#define _RATCC_UTIME_H

#if defined(_WIN32)
#error "<utime.h> is not available when targeting windows"
#endif

#include <sys/types.h>

struct utimbuf {
	time_t actime;
	time_t modtime;
};

int utime(const char* path, const struct utimbuf* times);

#endif
