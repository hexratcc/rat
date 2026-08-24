// expect: 100
// No matching case and no default leaves the prior value untouched.
int main(void) {
	int x = 7;
	int r = 100;
	switch (x) {
	case 1:
		r = 1;
		break;
	case 2:
		r = 2;
		break;
	}
	return r;
}
