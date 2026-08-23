#ifndef _RATCC_SYS_RESOURCE_H
#define _RATCC_SYS_RESOURCE_H

#if defined(_WIN32)
#error "<sys/resource.h> is not available when targeting windows"
#endif

#include <sys/time.h>
#include <sys/types.h>

typedef unsigned long rlim_t;

#define RLIM_INFINITY ((rlim_t) - 1)
#define RLIM_SAVED_MAX RLIM_INFINITY
#define RLIM_SAVED_CUR RLIM_INFINITY

#define RLIMIT_CPU 0
#define RLIMIT_FSIZE 1
#define RLIMIT_DATA 2
#define RLIMIT_STACK 3
#define RLIMIT_CORE 4
#define RLIMIT_RSS 5
#define RLIMIT_NPROC 6
#define RLIMIT_NOFILE 7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS 9

#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD 1

#define PRIO_PROCESS 0
#define PRIO_PGRP 1
#define PRIO_USER 2

struct rlimit {
	rlim_t rlim_cur;
	rlim_t rlim_max;
};

/* glibc x86-64 layout: every counter is a long after the two timevals */
struct rusage {
	struct timeval ru_utime;
	struct timeval ru_stime;
	long ru_maxrss;
	long ru_ixrss;
	long ru_idrss;
	long ru_isrss;
	long ru_minflt;
	long ru_majflt;
	long ru_nswap;
	long ru_inblock;
	long ru_oublock;
	long ru_msgsnd;
	long ru_msgrcv;
	long ru_nsignals;
	long ru_nvcsw;
	long ru_nivcsw;
};

int getrlimit(int resource, struct rlimit* out);
int setrlimit(int resource, const struct rlimit* val);
int getrusage(int who, struct rusage* out);
int getpriority(int which, id_t who);
int setpriority(int which, id_t who, int prio);

#endif
