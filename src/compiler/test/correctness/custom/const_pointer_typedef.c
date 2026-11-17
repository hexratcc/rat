// expect: 0
// const on a pointer typedef qualifies the pointer, not the pointee
typedef int* IntPtr;

int main(void) {
	int a = 1;
	const IntPtr p = &a;
	*p = 5;
	return a == 5 ? 0 : 1;
}
