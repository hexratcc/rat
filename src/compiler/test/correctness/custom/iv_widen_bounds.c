// expect: 0
// counted int loops whose IV strengthreduce widens to 64 bits, next to ones it must leave alone

int a[64];

__attribute__((noinline)) int sumStep(void) { // widened: 0 <= i < 64, step 4, i + 1..3
	int s = 0;
	for(int i = 0; i < 64; i += 4)
		s += a[i] + a[i + 1] * 2 + a[i + 2] * 3 + a[i + 3] * 4;
	return s;
}

__attribute__((noinline)) int sumLe(void) { // widened: i <= 62, i - 1 with i >= 1
	int s = 0;
	for(int i = 1; i <= 62; i++)
		s += a[i - 1] - a[i + 1];
	return s;
}

__attribute__((noinline)) int nearMax(void) { // i stays below INT_MAX, but i + 3 at the exit value would not
	int s = 0;
	for(int i = 2147483647 - 40; i < 2147483647 - 3; i += 4)
		s += a[(i + 3) - (2147483647 - 40)];
	return s;
}

__attribute__((noinline)) int empty(void) { // the back edge never runs
	int s = 0;
	for(int i = 100; i < 64; i++)
		s += a[i - 100];
	return s;
}

int main(void) {
	for(int i = 0; i < 64; i++)
		a[i] = i * 7 + 1;
	int ref = 0;
	for(int i = 0; i < 64; i += 4)
		ref += a[i] + a[i + 1] * 2 + a[i + 2] * 3 + a[i + 3] * 4;
	if(sumStep() != ref)
		return 1;
	if(sumLe() != -14 * 62)
		return 2;
	if(nearMax() != a[3] + a[7] + a[11] + a[15] + a[19] + a[23] + a[27] + a[31] + a[35] + a[39])
		return 3;
	if(empty() != 0)
		return 4;
	return 0;
}
