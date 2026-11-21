// expect: 0

// a pointer converted to _Bool tests against null, also without a cast
static _Bool nonNull(void* p) { return p; }

int main(void) {
	static char buf[16];
	char* p = (char*)(((unsigned long)buf | 255) + 1); // non-null, low byte zero
	_Bool b = p;
	return !b || !nonNull(p);
}
