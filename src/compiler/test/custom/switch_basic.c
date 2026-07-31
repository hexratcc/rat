// expect: 20
// A matching case runs its body and breaks out of the switch.
int main(void) {
	int x = 2;
	int r = 0;
	switch (x) {
	case 1:
		r = 10;
		break;
	case 2:
		r = 20;
		break;
	case 3:
		r = 30;
		break;
	default:
		r = 99;
	}
	return r;
}
