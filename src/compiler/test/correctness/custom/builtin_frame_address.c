// expect: 0

#define NOINLINE __attribute__((noinline))

NOINLINE void* leaf(void) { return __builtin_return_address(0); }
NOINLINE void* deep(void) { return __builtin_return_address(1); }

NOINLINE void* callerA(void) { return leaf(); }
NOINLINE void* callerB(void) { return leaf(); }

// the return address has to land inside the calling function's own code
static int inside(const void* ret, const void* fn) {
	unsigned long r = (unsigned long)ret, f = (unsigned long)fn;
	return r > f && r - f < 4096ul;
}

NOINLINE int between(const char* caller, const char* frame) {
	const char probe = 0;
	if(caller >= &probe)
		return caller >= frame && frame >= &probe;
	return caller <= frame && frame <= &probe;
}

NOINLINE int mid(const char* caller) {
	const char* f = __builtin_frame_address(0);
	// the comparison keeps this from being a tail call, which would drop the frame
	return between(caller, f) != 0;
}

NOINLINE int outer(void) {
	const char here = 0;
	return mid(&here) != 0;
}

int main(void) {
	// the same call site answers the same address every time
	if(callerA() != callerA())
		return 1;
	// two different call sites do not
	if(callerA() == callerB())
		return 2;
	// and each one points into its own caller
	if(!inside(callerA(), (const void*)callerA))
		return 3;
	if(!inside(callerB(), (const void*)callerB))
		return 4;
	// deeper levels are unknowable here
	if(deep() != 0)
		return 5;
	if(__builtin_frame_address(1) != 0)
		return 6;
	if(__builtin_frame_address(0) == 0)
		return 7;
	if(!outer())
		return 8;
	return 0;
}
