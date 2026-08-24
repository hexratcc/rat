// expect: 132

volatile int one = 1;

int main(void) {
	if(one)
		__builtin_trap();
	return 0;
}
