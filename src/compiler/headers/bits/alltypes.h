/* struct timespec is spelled in terms of time_t, so pull it in first */
#if defined(__NEED_struct_timespec) && !defined(__NEED_time_t)
#define __NEED_time_t
#endif

#if defined(__NEED_size_t) && !defined(__rat_defined_size_t)
#define __rat_defined_size_t
#if defined(_WIN32)
typedef unsigned long long size_t;
#else
typedef unsigned long size_t;
#endif
#endif

#if defined(__NEED_ptrdiff_t) && !defined(__rat_defined_ptrdiff_t)
#define __rat_defined_ptrdiff_t
#if defined(_WIN32)
typedef long long ptrdiff_t;
#else
typedef long ptrdiff_t;
#endif
#endif

#if defined(__NEED_wchar_t) && !defined(__rat_defined_wchar_t)
#define __rat_defined_wchar_t
#if defined(_WIN32)
typedef unsigned short wchar_t;
#else
typedef int wchar_t;
#endif
#endif

#if defined(__NEED_wint_t) && !defined(__rat_defined_wint_t)
#define __rat_defined_wint_t
#if defined(_WIN32)
typedef unsigned short wint_t;
#else
typedef unsigned int wint_t;
#endif
#endif

#if defined(__NEED_va_list) && !defined(__rat_defined_va_list)
#define __rat_defined_va_list
typedef __builtin_va_list va_list;
#endif

#if defined(__NEED_FILE) && !defined(__rat_defined_FILE)
#define __rat_defined_FILE
typedef struct __rat_FILE FILE;
#endif

#if defined(__NEED_ssize_t) && !defined(__rat_defined_ssize_t)
#define __rat_defined_ssize_t
#if defined(_WIN32)
typedef long long ssize_t;
#else
typedef long ssize_t;
#endif
#endif

#if defined(__NEED_off_t) && !defined(__rat_defined_off_t)
#define __rat_defined_off_t
#if defined(_WIN32)
typedef long long off_t;
#else
typedef long off_t;
#endif
#endif

#if defined(__NEED_time_t) && !defined(__rat_defined_time_t)
#define __rat_defined_time_t
#if defined(_WIN32)
typedef long long time_t;
#else
typedef long time_t;
#endif
#endif

#if defined(__NEED_clock_t) && !defined(__rat_defined_clock_t)
#define __rat_defined_clock_t
typedef long clock_t;
#endif

#if defined(__NEED_struct_timespec) && !defined(__rat_defined_struct_timespec)
#define __rat_defined_struct_timespec
struct timespec {
	time_t tv_sec;
	long tv_nsec;
};
#endif
