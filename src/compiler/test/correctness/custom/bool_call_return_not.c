// expect: 0
// passes: fold

typedef unsigned long long U64;

static _Bool eq(U64 a, U64 b) { return a == b; }

static U64 bangeq(U64 a, U64 b) { return !eq(a, b) ? 3 : 2; }

int main(void) {
	if(bangeq(7, 7) != 2)
		return 1;
	if(bangeq(7, 8) != 3)
		return 2;
	return 0;
}
