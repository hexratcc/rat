// expect: 3

typedef int T;

int main(void) {
	int s = 0;
	for(int T = 0; T < 3; T++)
		s += 1;
	T x = s;
	return x;
}
