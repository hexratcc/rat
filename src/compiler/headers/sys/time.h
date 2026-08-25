#ifndef _RATCC_SYS_TIME_H
#define _RATCC_SYS_TIME_H

#if defined(_WIN32)
#error "<sys/time.h> is not available when targeting windows"
#endif

#include <sys/types.h>

struct timeval {
	time_t tv_sec;
	suseconds_t tv_usec;
};

struct timezone {
	int tz_minuteswest;
	int tz_dsttime;
};

struct itimerval {
	struct timeval it_interval;
	struct timeval it_value;
};

#define ITIMER_REAL 0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF 2

#define timerisset(t) ((t)->tv_sec || (t)->tv_usec)
#define timerclear(t) ((t)->tv_sec = (t)->tv_usec = 0)
#define timercmp(a, b, op)                                                                         \
	((a)->tv_sec == (b)->tv_sec ? (a)->tv_usec op(b)->tv_usec : (a)->tv_sec op(b)->tv_sec)

int gettimeofday(struct timeval* tv, void* tz);
int settimeofday(const struct timeval* tv, const struct timezone* tz);
int getitimer(int which, struct itimerval* out);
int setitimer(int which, const struct itimerval* val, struct itimerval* old);
int utimes(const char* path, const struct timeval times[2]);

#endif
