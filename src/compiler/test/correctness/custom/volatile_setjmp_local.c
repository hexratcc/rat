// expect: 7
// a volatile local keeps its value across longjmp, so it must live in memory
#include <setjmp.h>

static jmp_buf env;

static void jump(void) { longjmp(env, 1); }

int main(void) {
	volatile int count = 0;
	if(setjmp(env) == 0) {
		count = 7;
		jump();
	}
	return count;
}
