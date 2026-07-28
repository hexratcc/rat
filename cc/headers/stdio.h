#ifndef _RATCC_STDIO_H
#define _RATCC_STDIO_H

#define __NEED_size_t
#define __NEED_va_list
#define __NEED_FILE
#include <bits/alltypes.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#if defined(_WIN32)
#define BUFSIZ 512
#define FILENAME_MAX 260
#define FOPEN_MAX 20
#define TMP_MAX 2147483647
#define L_tmpnam 260
#else
#define BUFSIZ 8192
#define FILENAME_MAX 4096
#define FOPEN_MAX 16
#define TMP_MAX 238328
#define L_tmpnam 20
#endif
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#if defined(_WIN32)
FILE* __acrt_iob_func(unsigned index);
#define stdin (__acrt_iob_func(0))
#define stdout (__acrt_iob_func(1))
#define stderr (__acrt_iob_func(2))
#else
extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;
#endif

FILE* fopen(const char* path, const char* mode);
FILE* freopen(const char* path, const char* mode, FILE* stream);
int fclose(FILE* stream);
int fflush(FILE* stream);
int remove(const char* path);
int rename(const char* oldpath, const char* newpath);
void setbuf(FILE* stream, char* buf);
int setvbuf(FILE* stream, char* buf, int mode, size_t size);

int printf(const char* fmt, ...);
int fprintf(FILE* stream, const char* fmt, ...);
int sprintf(char* buf, const char* fmt, ...);
int snprintf(char* buf, size_t size, const char* fmt, ...);
int vprintf(const char* fmt, va_list ap);
int vfprintf(FILE* stream, const char* fmt, va_list ap);
int vsprintf(char* buf, const char* fmt, va_list ap);
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);

int scanf(const char* fmt, ...);
int fscanf(FILE* stream, const char* fmt, ...);
int sscanf(const char* str, const char* fmt, ...);

int fgetc(FILE* stream);
int getc(FILE* stream);
int getchar(void);
int ungetc(int c, FILE* stream);
int fputc(int c, FILE* stream);
int putc(int c, FILE* stream);
int putchar(int c);
char* fgets(char* buf, int size, FILE* stream);
int fputs(const char* s, FILE* stream);
int puts(const char* s);

size_t fread(void* ptr, size_t size, size_t count, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t count, FILE* stream);

int fseek(FILE* stream, long offset, int whence);
long ftell(FILE* stream);
void rewind(FILE* stream);

int feof(FILE* stream);
int ferror(FILE* stream);
void clearerr(FILE* stream);
void perror(const char* prefix);

FILE* tmpfile(void);

#endif
