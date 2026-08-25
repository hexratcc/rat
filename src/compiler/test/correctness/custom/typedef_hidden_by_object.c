// expect: 0

int main(void) {
	typedef int t;
	t t = 5;
	int r = ({ t; });
	return r == 5 ? 0 : 1;
}
