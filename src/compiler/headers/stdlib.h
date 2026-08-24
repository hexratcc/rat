#ifndef _RATCC_STDLIB_H
#define _RATCC_STDLIB_H

#define __NEED_size_t
#define __NEED_wchar_t
#include <bits/alltypes.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#if defined(_WIN32)
#define RAND_MAX 0x7fff
#else
#define RAND_MAX 2147483647
#endif

void* malloc(size_t size);
int posix_memalign(void** memptr, size_t alignment, size_t size);
void* calloc(size_t count, size_t size);
void* realloc(void* ptr, size_t size);
void free(void* ptr);

void abort(void);
void exit(int status);
void _Exit(int status);
int atexit(void (*fn)(void));
char* getenv(const char* name);
int system(const char* command);

int atoi(const char* s);
long atol(const char* s);
long long atoll(const char* s);
double atof(const char* s);
long strtol(const char* s, char** end, int base);
unsigned long strtoul(const char* s, char** end, int base);
long long strtoll(const char* s, char** end, int base);
unsigned long long strtoull(const char* s, char** end, int base);
float strtof(const char* s, char** end);
double strtod(const char* s, char** end);
long double strtold(const char* s, char** end);

int rand(void);
void srand(unsigned seed);

void qsort(void* base, size_t count, size_t size, int (*cmp)(const void*, const void*));
void* bsearch(const void* key,
							const void* base,
							size_t count,
							size_t size,
							int (*cmp)(const void*, const void*));

int abs(int n);
long labs(long n);
long long llabs(long long n);

#endif
