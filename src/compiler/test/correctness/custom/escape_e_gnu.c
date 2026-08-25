// expect: 0
int main(void) {
	if("\e"[0] != 27)
		return 1;
	if('\e' != 27)
		return 2;
	return 0;
}
