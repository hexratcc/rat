// expect: 99
// With no matching case, control transfers to default.
int main(void) {
	int x = 5;
	int r = 0;
	switch (x) {
	case 1:
		r = 10;
		break;
	case 2:
		r = 20;
		break;
	default:
		r = 99;
	}
	return r;
}
