#ifndef _RATCC_SYS_IOCTL_H
#define _RATCC_SYS_IOCTL_H

#if defined(_WIN32)
#error "<sys/ioctl.h> is not available when targeting windows"
#endif

#define TCGETS 0x5401
#define TCSETS 0x5402
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD 0x541B
#define FIONBIO 0x5421
#define FIOCLEX 0x5451
#define FIONCLEX 0x5450

struct winsize {
	unsigned short ws_row;
	unsigned short ws_col;
	unsigned short ws_xpixel;
	unsigned short ws_ypixel;
};

int ioctl(int fd, unsigned long request, ...);

#endif
