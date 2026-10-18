// expect: 0
// memoryopt lets the loads after the loop read the state before it; that state has no writer in
// their block, so one new vector load of it could run after the store to A[0]
unsigned A[64], B[64];
unsigned char RAW[512];

__attribute__((noinline)) void f(int n) {
	for(int i = 0; i < n; i++)
		RAW[i] = (unsigned char)i;
	unsigned a0 = A[0], a1 = A[1], a2 = A[2], a3 = A[3];
	A[0] = 7;
	B[0] = a0;
	B[1] = a1;
	B[2] = a2;
	B[3] = a3;
}

int main(void) {
	for(int i = 0; i < 64; i++)
		A[i] = i + 100;
	f(100);
	if(A[0] != 7 || B[0] != 100 || B[3] != 103)
		return 1;
	return 0;
}
