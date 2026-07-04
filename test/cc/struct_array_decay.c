// expect: 7
// An array member decays to a pointer when passed to a function.
struct Buf { int data[3]; };

int sum3(int *p) {
	return p[0] + p[1] + p[2];
}

int main(void) {
	struct Buf b;
	b.data[0] = 1;
	b.data[1] = 2;
	b.data[2] = 4;
	return sum3(b.data);
}
