// expect: 0
int true_fn() { return 513; }
int char_fn() { return (2 << 8) + 3; }

_Bool true_fn();
char char_fn();

int main(void) {
	if(true_fn() != 1)
		return 1;
	if(char_fn() != 3)
		return 2;
	return 0;
}
