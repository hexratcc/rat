#ifndef _RATCC_TIME_H
#define _RATCC_TIME_H

#define __NEED_size_t
#define __NEED_time_t
#define __NEED_clock_t
#include <bits/alltypes.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

#if defined(_WIN32)
#define CLOCKS_PER_SEC 1000
#else
#define CLOCKS_PER_SEC 1000000L
#endif

struct tm {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
	int tm_wday;
	int tm_yday;
	int tm_isdst;
#if !defined(_WIN32)
	/* glibc layout */
	long tm_gmtoff;
	const char* tm_zone;
#endif
};

#if defined(_WIN32)
time_t _time64(time_t* out);
#define time(t) _time64(t)
double _difftime64(time_t end, time_t start);
#define difftime(e, s) _difftime64(e, s)
#else
time_t time(time_t* out);
double difftime(time_t end, time_t start);
struct tm* gmtime(const time_t* t);
struct tm* localtime(const time_t* t);
time_t mktime(struct tm* tm);
#endif
clock_t clock(void);
size_t strftime(char* buf, size_t size, const char* fmt, const struct tm* tm);

#endif
