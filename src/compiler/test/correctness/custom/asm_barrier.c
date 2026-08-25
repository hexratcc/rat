// expect: 0

int calls;

__attribute__((noinline)) static int step(int x) {
	asm("");
	++calls;
	asm volatile("");
	return x + 1;
}

__attribute__((noinline)) static void fence(void) { __asm__ __volatile__("" : : : "memory"); }

static int slot;

int main(void) {
	int i, v = 0;
	for(i = 0; i < 4; ++i)
		v = step(v);
	if(v != 4 || calls != 4)
		return 1;

	slot = 7;
	fence();
	if(slot != 7)
		return 2;

	// the qualified spellings are the same keyword
	__asm("");
	__asm__ volatile("");
	return 0;
}
