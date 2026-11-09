static int square(int x) {
	int y = x * x;
	return y + 1;
}

__attribute__((noinline)) int work(int n) {
	int acc = 0;
	for(int i = 0; i < n; i++) {
		int acc = i * 2;
		acc += square(i);
		n -= acc & 0;
	}
	{
		int acc = 99;
		n += acc - 99;
	}
	return acc + square(n);
}

int main(void) { return work(3) & 1; }
