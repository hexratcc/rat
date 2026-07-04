// expect: 7
// Without break, execution falls through to the next case body.
int main(void) {
	int x = 1;
	int r = 0;
	switch (x) {
	case 1:
		r = r + 1;
	case 2:
		r = r + 2;
	case 3:
		r = r + 4;
		break;
	case 4:
		r = r + 100;
	}
	return r;
}
