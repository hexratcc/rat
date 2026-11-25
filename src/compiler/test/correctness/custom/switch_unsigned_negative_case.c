// expect: 0
// passes:
// a case label converts to the promoted type of the controlling expression
int f(int x) {
	switch(x) {
	case 0xFFFFFFFFu: // -1 here, it sorts below the other cases
		return 1;
	case 0:
	case 1:
	case 2:
	case 3:
		return 2;
	}
	return 0;
}

// the cases span all 64 bits
int g(unsigned long long x) {
	switch(x) {
	case 0:
	case 1:
	case 2:
	case 3:
	case 4:
	case -1ull:
		return 1;
	}
	return 0;
}

int main(void) { return f(-1) != 1 || g(-1ull) != 1; }
