// expect: 0
// passes:

typedef struct {
	char pad[16];
} Big;

int helper6(int a, int b, int c, int d, int e, int f) { return a + b + c + d + e + f; }

int helper7(int a, int b, int c, int d, int e, int f, int g) { return a + b + c + d + e + f + g; }

int check(void* st, const char* src, char* dst, int a, int b, int c) {
	*dst = st == (const void*)src ? 'X' : 'o';
	return a + b + c;
}

int warmA(int a, int b, int c, int d, int e, int f) { return helper6(a, b, c, d, e, f); }

int warmB(int a, int b, int c, int d, int e, int f) { return helper7(a, b, c, d, e, f, 1); }

int mid(const char* src, char* dst, int a, int b, int c) {
	int result;
	Big ctx;
	Big* const ctxPtr = &ctx;
	result = check(ctxPtr, src, dst, a, b, c);
	return result;
}

int main(void) {
	static const char s[] = "hello";
	char out = '?';

	if(warmA(1, 2, 3, 4, 5, 6) != 21)
		return 2;
	if(warmB(1, 2, 3, 4, 5, 6) != 22)
		return 3;
	if(mid(s, &out, 1, 2, 3) != 6)
		return 4;
	return out == 'o' ? 0 : 1;
}
