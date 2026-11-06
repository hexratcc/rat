// expect: 1
// passes:

// a negative int read back from memory, converted to long double
long double widen(int s) {
	int* p = &s;
	return *p;
}

int main(void) { return widen(-1) == -1.0L; }
