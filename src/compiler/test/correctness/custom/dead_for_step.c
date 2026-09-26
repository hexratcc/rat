// expect: 42
// passes:

int g = 40;

int first(void) {
	int x = 0;
	for(;; x++)
		return g + x;
}

int second(int n) {
	int i, s = 0;
	for(i = 0; i < n; i += s) {
		s += i;
		if(s > 0)
			break;
		return s + 2;
	}
	return -1;
}

int main(void) { return first() + second(3); }
