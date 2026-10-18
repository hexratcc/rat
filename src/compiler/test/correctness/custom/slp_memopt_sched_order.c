// expect: 0
// memoryopt lets loads after a loop that leaves A and B alone read the state before the loop, while
// the stores to A and B after them consume the loop's state: the new vector loads of a reduction
// (k0) and of an 8-byte store window (k2) must still run before those stores
unsigned A[64], B[64], G[64];

__attribute__((noinline)) unsigned k0(unsigned* p, unsigned* q, int i) {
	unsigned L[64];
	for(int j = 0; j < 64; j++)
		L[j] = G[j];
	unsigned t0 = B[5] + q[19] + B[7] + B[8] + B[9] + B[10] + B[11] + B[12] + B[13] + B[14] + B[15] + B[16] + B[17];
	unsigned t1 = p[6];
	unsigned t2 = q[20];
	B[i + 9] = L[16] + p[13];
	unsigned t3 = B[2] + B[3] + B[4] + B[5] + B[6] + B[7] + B[8] + B[9] + B[10] + B[11];
	unsigned t4 = q[21] + q[1] + q[2] + q[3] + q[9] + q[5] + q[6] + q[7] + q[8] + q[9] + q[5] + L[22] + q[12] + q[13] + q[14];
	unsigned t5 = q[8] + q[9] + q[10] + q[11] + q[5] + q[13] + q[14] + q[15] + q[16] + q[17] + q[18] + q[19];
	unsigned t6 = B[5] + B[6] + p[4] + q[23] + B[9] + B[10] + L[3] + B[12] + A[20] + q[11] + A[2] + p[6] + L[20] + B[18];
	return 1 + t0 * 89u + t1 * 55u + t2 * 63u + t3 * 93u + t4 * 73u + t5 * 17u + t6 * 87u;
}

__attribute__((noinline)) unsigned k1(unsigned* p, int i) {
	unsigned L[64];
	for(int j = 0; j < 64; j++)
		L[j] = G[j];
	unsigned t0 = B[4] + B[5] + B[6] + B[7] + p[1] + B[9] + L[23] + B[11] + B[12] + B[13] + B[14] + B[15];
	p[i + 6] = A[11] ^ B[16];
	unsigned t1 = A[15];
	unsigned t2 = L[2];
	return 1 + t0 * 29u + t1 * 21u + t2 * 61u;
}

__attribute__((noinline)) unsigned k2(unsigned* p, unsigned* q, int i) {
	unsigned L[64];
	for(int j = 0; j < 64; j++)
		L[j] = G[j];
	A[21] = A[23] * A[16];
	A[20] = A[22] * A[15];
	A[15] = A[17] * A[10];
	unsigned t0 = q[23];
	unsigned t1 = L[16];
	unsigned t2 = L[i + 11];
	unsigned t3 = A[i + 1];
	unsigned t4 = p[10];
	unsigned t5 = L[4];
	unsigned t6 = L[2];
	return 1 + t0 * 73u + t1 * 69u + t2 * 31u + t3 * 51u + t4 * 9u + t5 * 5u + t6 * 39u;
}

static void init(void) {
	for(int j = 0; j < 64; j++) {
		A[j] = j * 2654435761u + 7;
		B[j] = j * 40503u + 3;
		G[j] = j ^ 0x5au;
	}
}

static unsigned sum(void) {
	unsigned h = 0;
	for(int j = 0; j < 64; j++)
		h = h * 31 + A[j] * 37 + B[j];
	return h;
}

int main(void) {
	init();
	unsigned h0 = k0(A + 32, B + 32, 3) + sum();
	init();
	unsigned h1 = k1(B + 4, 2) + sum();
	init();
	unsigned h2 = k2(A + 32, B + 32, 3) + sum();
	return (h0 != 540407866u) | (h1 != 1010691610u) << 1 | (h2 != 3195658356u) << 2;
}
