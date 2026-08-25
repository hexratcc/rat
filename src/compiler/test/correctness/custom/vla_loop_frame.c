// expect: 0

volatile int n = 4000;
void *volatile sink;

int main(void) {
	char *first = 0;
	int i;
	for (i = 0; i < 200000; i = i + 1) {
		char v[n];
		v[0] = 1;
		v[n - 1] = 2;
		sink = v;
		if (i == 0)
			first = v;
		// the block is re-entered from the same stack level every time
		else if (v != first)
			return 1;
	}
	return 0;
}
