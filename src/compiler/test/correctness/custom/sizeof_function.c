// expect: 0
int f(void) { return 0; }

int main(void) {
	if(sizeof(f) != 1)
		return 1;
	if(sizeof(main) != 1)
		return 2;
	return 0;
}
