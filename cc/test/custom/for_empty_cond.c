// expect: 5
// for with an omitted condition loops until break.
int main(void) {
	int i = 0;
	for (;;) {
		if (i == 5)
			break;
		i += 1;
	}
	return i;
}
