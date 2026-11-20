// expect: 0

static int fromDouble[(int)((double)3 / 2 * 2)];
static int eqUnsigned = -1 == 0xFFFFFFFFu;

int main(void) {
	if(sizeof(fromDouble) != 3 * sizeof(int))
		return 1;
	if(eqUnsigned != 1)
		return 2;
	return 0;
}
