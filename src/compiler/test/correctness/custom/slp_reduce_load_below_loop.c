// expect: 0
// the loop leaves B alone, so the B loads after it read the entry state, while the store to B
// after them consumes the loop's state: the wide loads of the add reduction over them read the
// entry state too, and the scheduler must keep them above the store
unsigned B[64], G[64];

__attribute__((noinline)) unsigned k(unsigned *q, int i) {
	unsigned L[64];
	for(int j = 0; j < 64; j++)
		L[j] = G[j];
	unsigned t = B[5] + q[19] + B[7] + B[8] + B[9] + B[10] + B[11] + B[12] + B[13] + B[14] + B[15] +
							 B[16] + B[17];
	B[i + 9] = L[16];
	return t;
}

int main(void) {
	unsigned want = 0;
	for(int j = 0; j < 64; j++) {
		B[j] = j * 40503u + 3;
		G[j] = j ^ 0x5au;
	}
	for(int j = 5; j <= 17; j++)
		want += j == 6 ? B[32 + 19] : B[j];
	if(k(B + 32, 3) != want)
		return 1;
	return 0;
}
