#ifndef _RATCC_SETJMP_H
#define _RATCC_SETJMP_H

typedef struct {
	long long __rat_jb[32];
} __rat_jmp_buf_record;
typedef __rat_jmp_buf_record jmp_buf[1];

#if defined(_WIN32)
int _setjmp(jmp_buf env, void* frame);
#define setjmp(env) _setjmp(env, 0)
#else
int setjmp(jmp_buf env);
#endif
void longjmp(jmp_buf env, int val);

#endif
