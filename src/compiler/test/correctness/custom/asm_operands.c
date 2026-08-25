// expect: 0

static int arr[4] = {10, 20, 30, 40};

int main(void) {
	int x = 41;
	asm("" : "+r"(x));
	if(x != 41)
		return 1;

	int y;
	asm("" : "=r"(y) : "0"(x));
	if(y != 41)
		return 2;

	// the laundered index is opaque, so the load cannot be folded to a constant
	int i = 2;
	asm("" : "+r"(i));
	if(arr[i] != 30)
		return 3;

	long l = -5;
	asm("" : "+r"(l));
	if(l != -5)
		return 4;

	int* p = &arr[1];
	asm("" : "+r"(p));
	if(*p != 20)
		return 5;

	double d = 1.5;
	asm("" : "+r"(d));
	if(d != 1.5)
		return 6;

	// two pairs at once, plus an input no output is tied to
	int a = 3, b = 4, keep = 9;
	asm volatile("" : "+r"(a), "+r"(b) : "r"(keep) : "memory");
	if(a != 3 || b != 4 || keep != 9)
		return 7;
	return 0;
}
