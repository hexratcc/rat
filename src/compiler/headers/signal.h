#ifndef _RATCC_SIGNAL_H
#define _RATCC_SIGNAL_H

typedef int sig_atomic_t;
typedef void (*__rat_sighandler_t)(int);

#define SIG_DFL ((__rat_sighandler_t)0)
#define SIG_IGN ((__rat_sighandler_t)1)
#define SIG_ERR ((__rat_sighandler_t)-1)

#if defined(_WIN32)
#define SIGINT 2
#define SIGILL 4
#define SIGFPE 8
#define SIGSEGV 11
#define SIGTERM 15
#define SIGBREAK 21
#define SIGABRT 22
#else
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#endif

__rat_sighandler_t signal(int signum, __rat_sighandler_t handler);
int raise(int signum);

#endif
