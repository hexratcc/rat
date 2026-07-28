#ifndef _RATCC_ERRNO_H
#define _RATCC_ERRNO_H

#if defined(_WIN32)
int* _errno(void);
#define errno (*_errno())
#define EDOM 33
#define ERANGE 34
#define EILSEQ 42
#else
int* __errno_location(void);
#define errno (*__errno_location())
#define EDOM 33
#define ERANGE 34
#define EILSEQ 84
#endif

#endif
