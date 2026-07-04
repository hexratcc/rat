// expect: 7

// a user function returning void*; the result is cast back to int* and used.
void *id(void *p) { return p; }

int main(void) {
	int x = 7;
	int *q = (int *)id(&x);
	return *q;
}
