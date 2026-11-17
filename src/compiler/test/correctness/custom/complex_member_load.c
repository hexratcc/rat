// expect: 3
struct S {
	double _Complex c;
	int k;
};

int main(void) {
	struct S s;
	s.c = 3.0;
	double _Complex d = s.c;
	return (int)__real__ d;
}
