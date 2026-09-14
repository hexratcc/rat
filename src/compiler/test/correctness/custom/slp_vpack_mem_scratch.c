// expect: 0
// a float vector pack gathers its lanes through the frame scratch slot
typedef float T;
T A[128], B[128], C[128];

void k(void) {
	A[8 + 7] = B[8 + 7] - C[8 + 7];
	A[8 + 6] = B[8 + 6] - C[8 + 6];
	A[8 + 5] = B[8 + 5] - C[8 + 5];
	A[8 + 3] = B[8 + 3] - C[8 + 3];
	{
		T L[16];
		L[0] = B[8 + 0] * C[8 + 0];
		L[1] = B[8 + 1] * C[8 + 1];
		L[2] = B[8 + 2] * C[8 + 2];
		L[3] = B[8 + 3] * C[8 + 3];
		L[4] = B[8 + 4] + C[8 + 4];
		L[5] = B[8 + 5] * C[8 + 5];
		L[6] = B[8 + 6] + C[8 + 6];
		L[7] = B[8 + 7] - C[8 + 7];
		A[0] = L[0] + 8.5f;
	}
	{
		T s0 = B[0], s1 = B[1], s2 = B[2], s3 = B[3];
		A[4 + 2] = s1 * C[4 + 2];
		A[4 + 3] = s1 * C[4 + 3];
		A[8 + 0] = s2 * C[8 + 0];
		A[8 + 1] = s2 * C[8 + 1];
		A[12 + 5] = s3 * C[12 + 5];
		A[2] = s0 * C[2];
	}
	{
		T L[16];
		L[0] = B[8 + 0] - C[8 + 0];
		L[1] = B[8 + 1] + C[8 + 1];
		A[0] = L[0] - 13.5f;
		A[1] = L[1] - 28.5f;
	}
}

int main(void) {
	for(int i = 0; i < 128; i++) {
		A[i] = (T)(i % 17);
		B[i] = (T)((i * 3) % 23);
		C[i] = (T)((i * 5) % 11);
	}
	k();
	// sum in a fixed-point integer so the check is bit-exact
	unsigned long total = 0;
	for(int i = 0; i < 32; i++)
		total = total * 3u + (unsigned long)(long)(A[i] * 16.0f);
	return total != 18178825671880576560ul;
}
