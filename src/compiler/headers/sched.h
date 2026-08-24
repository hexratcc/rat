#ifndef _RATCC_SCHED_H
#define _RATCC_SCHED_H

#if defined(_WIN32)
#error "<sched.h> is not available when targeting windows"
#endif

#include <sys/types.h>

/* linux scheduling policies */
#define SCHED_OTHER 0
#define SCHED_FIFO 1
#define SCHED_RR 2
#define SCHED_BATCH 3
#define SCHED_IDLE 5

struct sched_param {
	int sched_priority;
};

int sched_get_priority_max(int policy);
int sched_get_priority_min(int policy);
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param);
int sched_getscheduler(pid_t pid);
int sched_setparam(pid_t pid, const struct sched_param *param);
int sched_getparam(pid_t pid, struct sched_param *param);
int sched_yield(void);

#endif
