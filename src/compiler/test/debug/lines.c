// flags: -g1
__attribute__((noinline)) int twice(int v) {
	int w = v * 2;
	return w;
}

int main(void) { return twice(21) & 1; }
