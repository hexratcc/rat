// expect: 0

double A[64], B[64], C[64];

void k(double* p, double* q) {
	p[23] = q[23] * B[23];
	C[12] = q[12];
	C[13] = q[13];
	p[12] = q[12] * B[12];
	p[13] = q[13] * B[13];
}

int main(void) {
	for(int i = 0; i < 64; i++) {
		A[i] = i + 1;
		B[i] = 2.0;
		C[i] = 0.0;
	}
	k(A, B);
	if(A[12] != 4.0 || A[13] != 4.0 || A[23] != 4.0)
		return 1;
	if(C[12] != 2.0 || C[13] != 2.0)
		return 2;
	return 0;
}
