#ifndef _RATCC_STDDEF_H
#define _RATCC_STDDEF_H

#define __NEED_size_t
#define __NEED_ptrdiff_t
#define __NEED_wchar_t
#include <bits/alltypes.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

typedef struct {
	long long __rat_ll;
	long double __rat_ld;
} max_align_t;

#endif
