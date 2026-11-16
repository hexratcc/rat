// expect: 0
// Braces around a scalar element initializer are allowed (C99 6.7.8p11).
int main(void) {
	int b[2] = {{1}, {2}};
	if(b[0] != 1)
		return 1;
	return b[1] != 2;
}
