// expect: 22
struct S {
	const unsigned char *a;
	const unsigned char *b;
};

int main(void) {
	static const unsigned char data[] = {1, 2, 3};
	static const unsigned char *p0 = data;
	static const unsigned char *p1 = data + 2;
	static const unsigned char *p2 = &data[2];
	static const char *p3 = (const char *)data;
	static struct S s = {data, data + 1};
	static const unsigned char *arr[2] = {data, &data[1]};
	static int sv = 7;
	static int *ps = &sv;
	// 1 + 3 + 3 + 2 + 1 + 2 + 1 + 2 + 7
	return (int)(p0[0] + *p1 + *p2 + p3[1] + *s.a + *s.b + *arr[0] + *arr[1] + *ps);
}
