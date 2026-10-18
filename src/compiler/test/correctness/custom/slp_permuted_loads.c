// expect: 0
// lanes that read one 16-byte block in another order, with duplicates, or every other element of two
// blocks pack as wide loads and a shuffle; a wide load reads a byte no lane reads only when another
// load of the block reads it, so the tuples at the end of a mapped page stay scalar
void* mmap(void*, unsigned long, int, int, int, long);
int mprotect(void*, unsigned long, int);

__attribute__((noinline)) void rev(float* restrict d, const float* restrict a, const float* restrict b) {
	d[0] = a[3] * b[3];
	d[1] = a[2] * b[2];
	d[2] = a[1] * b[1];
	d[3] = a[0] * b[0];
}

__attribute__((noinline)) void swap(int* restrict d, const int* restrict a, const int* restrict b) {
	d[0] = a[1] + b[0];
	d[1] = a[0] + b[1];
	d[2] = a[3] + b[2];
	d[3] = a[2] + b[3];
}

__attribute__((noinline)) void deint(float* restrict d, const float* restrict a) {
	d[0] = a[0] - a[1];
	d[1] = a[2] - a[3];
	d[2] = a[4] - a[5];
	d[3] = a[6] - a[7];
}

__attribute__((noinline)) void cmul(float* restrict c, const float* restrict a, const float* restrict b) {
	c[0] = a[0] * b[0] - a[1] * b[1];
	c[1] = a[0] * b[1] + a[1] * b[0];
	c[2] = a[2] * b[2] - a[3] * b[3];
	c[3] = a[2] * b[3] + a[3] * b[2];
}

__attribute__((noinline)) void rev64(long long* restrict d, const long long* restrict a,
																		 const long long* restrict b) {
	d[0] = a[1] ^ b[1];
	d[1] = a[0] ^ b[0];
}

__attribute__((noinline)) void swap2(int* restrict d, const int* restrict a, const int* restrict b) {
	d[0] = a[1] * b[1];
	d[1] = a[0] * b[0];
}

// only the even elements are read: no wide load may touch the odd ones past the page end
__attribute__((noinline)) void evens(int* restrict d, const int* restrict a) {
	d[0] = a[0] + 1;
	d[1] = a[2] + 2;
	d[2] = a[4] + 3;
	d[3] = a[6] + 4;
}

__attribute__((noinline)) void dups(int* restrict d, const int* restrict a) {
	d[0] = a[0] * 2;
	d[1] = a[0] * 3;
	d[2] = a[2] * 4;
	d[3] = a[2] * 5;
}

int main(void) {
	float a[8], b[4], d[4];
	int ia[8], ib[4], id[4];
	for(int i = 0; i < 8; i++) {
		a[i] = (float)(i * 3 + 1);
		ia[i] = i * i - 7;
	}
	for(int i = 0; i < 4; i++) {
		b[i] = (float)(5 - i);
		ib[i] = 3 * i + 11;
	}
	rev(d, a, b);
	for(int i = 0; i < 4; i++)
		if(d[i] != a[3 - i] * b[3 - i])
			return 1;
	swap(id, ia, ib);
	for(int i = 0; i < 4; i++)
		if(id[i] != ia[i ^ 1] + ib[i])
			return 2;
	deint(d, a);
	for(int i = 0; i < 4; i++)
		if(d[i] != a[2 * i] - a[2 * i + 1])
			return 3;
	cmul(d, a, b);
	if(d[0] != a[0] * b[0] - a[1] * b[1] || d[1] != a[0] * b[1] + a[1] * b[0] ||
		 d[2] != a[2] * b[2] - a[3] * b[3] || d[3] != a[2] * b[3] + a[3] * b[2])
		return 4;
	long long la[2] = {123456789012LL, -42}, lb[2] = {77, -987654321098LL}, ld[2];
	rev64(ld, la, lb);
	if(ld[0] != (la[1] ^ lb[1]) || ld[1] != (la[0] ^ lb[0]))
		return 5;
	swap2(id, ia, ib);
	if(id[0] != ia[1] * ib[1] || id[1] != ia[0] * ib[0])
		return 6;
	char* m = mmap(0, 8192, 3, 0x22, -1, 0);
	mprotect(m + 4096, 4096, 0);
	int* end = (int*)(m + 4096);
	int* ev = end - 7;
	for(int i = 0; i < 7; i++)
		ev[i] = 10 * i;
	evens(id, ev);
	for(int i = 0; i < 4; i++)
		if(id[i] != ev[2 * i] + i + 1)
			return 7;
	int* du = end - 3;
	du[0] = 6;
	du[2] = -8;
	dups(id, du);
	if(id[0] != 12 || id[1] != 18 || id[2] != -32 || id[3] != -40)
		return 8;
	return 0;
}
