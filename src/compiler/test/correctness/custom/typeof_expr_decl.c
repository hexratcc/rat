// expect: 0

double g = 1.5;
struct S {
	__typeof__(g) m;
	char c;
};

int main(void) {
	double d = 1.5;
	__typeof__(d) x = 2.5;
	__typeof__(d) arr[3] = {0.5, 1.5, 2.5};
	void* v = &d;
	if(sizeof(x) != sizeof(double) || x != 2.5)
		return 1;
	if(sizeof(arr) != 3 * sizeof(double) || arr[2] != 2.5)
		return 2;
	if(sizeof(struct S) != 16)
		return 3;
	if(*(__typeof__(d)*)v != 1.5)
		return 4;
	return 0;
}
