// expect: 2

// #if / #elif / #else selection driven by a macro value.
#define MODE 2

#if MODE == 1
int pick(void) { return 10; }
#elif MODE == 2
int pick(void) { return 2; }
#else
int pick(void) { return 99; }
#endif

int main(void) {
	return pick();
}
