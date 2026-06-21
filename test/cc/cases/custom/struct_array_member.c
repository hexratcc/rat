// expect: 100
// An array member is indexed and summed within a loop.
struct Buf { int len; int data[4]; };

int main(void) {
	struct Buf b;
	b.len = 4;
	b.data[0] = 10;
	b.data[1] = 20;
	b.data[2] = 30;
	b.data[3] = 40;
	int sum = 0;
	for (int i = 0; i < b.len; i++)
		sum += b.data[i];
	return sum;
}
