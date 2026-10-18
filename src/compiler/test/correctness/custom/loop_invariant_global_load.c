// expect: 0
// a load of a global that no store of the loop aliases reads the loop-entry state and moves out of
// the loop; a global the loop writes, and a loop that never runs, keep their meaning

unsigned X[64], Y[64], k = 3, n = 0;

__attribute__((noinline)) void scale(int m) { // k is invariant
	for(int i = 0; i < m; i++)
		Y[i] = k * X[i];
}

__attribute__((noinline)) void count(int m) { // n is written in the loop
	for(int i = 0; i < m; i++) {
		Y[i] = n;
		n = n + 1;
	}
}

int main(void) {
	for(int i = 0; i < 64; i++)
		X[i] = i + 1;
	scale(0);
	if(Y[0] != 0)
		return 1;
	scale(64);
	for(int i = 0; i < 64; i++)
		if(Y[i] != 3u * (i + 1))
			return 2;
	k = 5;
	scale(64);
	if(Y[63] != 5u * 64)
		return 3;
	count(64);
	for(int i = 0; i < 64; i++)
		if(Y[i] != (unsigned)i)
			return 4;
	return n == 64 ? 0 : 5;
}
