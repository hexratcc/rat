#ifndef _RATCC_WCHAR_H
#define _RATCC_WCHAR_H

#define __NEED_size_t
#define __NEED_wchar_t
#define __NEED_wint_t
#define __NEED_FILE
#include <bits/alltypes.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifndef WCHAR_MIN
#if defined(_WIN32)
#define WCHAR_MIN 0
#define WCHAR_MAX 65535
#else
#define WCHAR_MIN (-2147483647 - 1)
#define WCHAR_MAX 2147483647
#endif
#endif

#if defined(_WIN32)
#define WEOF ((wint_t)0xffff)
#else
#define WEOF 0xffffffffU
#endif

size_t wcslen(const wchar_t* s);
int wcscmp(const wchar_t* a, const wchar_t* b);
int wcsncmp(const wchar_t* a, const wchar_t* b, size_t n);
wchar_t* wcscpy(wchar_t* dst, const wchar_t* src);
wchar_t* wcsncpy(wchar_t* dst, const wchar_t* src, size_t n);
wchar_t* wcscat(wchar_t* dst, const wchar_t* src);
wchar_t* wcschr(const wchar_t* s, wchar_t c);
wchar_t* wmemcpy(wchar_t* dst, const wchar_t* src, size_t n);
wchar_t* wmemset(wchar_t* dst, wchar_t c, size_t n);
int wmemcmp(const wchar_t* a, const wchar_t* b, size_t n);

#endif
