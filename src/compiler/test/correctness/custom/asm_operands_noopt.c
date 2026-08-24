// expect: 0
// passes:

static int arr[4] = {10, 20, 30, 40};

int main(void) {
	int x = 41;
	asm("" : "+r"(x));
	if(x != 41)
		return 1;

	int y;
	asm("" : "=r"(y) : "0"(x + 1));
	if(y != 42)
		return 2;

	int i = 2;
	asm volatile("" : "+r"(i) : : "memory");
	if(arr[i] != 30)
		return 3;

	unsigned long long u = 0x1122334455667788ull;
	asm("" : "+r"(u));
	if(u != 0x1122334455667788ull)
		return 4;

	float f = 2.5f;
	asm("" : "+r"(f));
	if(f != 2.5f)
		return 5;
	return 0;
}
