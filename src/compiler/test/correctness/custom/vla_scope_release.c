// expect: 0

void* volatile p;

static void byBlock(void) {
	int n = 0;
	while(n < 200000) {
		int x[n % 500 + 1];
		x[0] = 1;
		p = x;
		n++;
	}
}

static void byGoto(void) {
	int n = 0;
lab:;
	int x[n % 500 + 1];
	x[0] = 1;
	p = x;
	n++;
	if(n < 200000)
		goto lab;
}

// the label sits in a nested scope, before the VLA of the enclosing one
static void byGotoInner(void) {
	int n = 0;
	if(0) {
	lab:;
	}
	int x[n % 500 + 1];
	x[0] = 1;
	p = x;
	n++;
	if(n < 200000)
		goto lab;
}

// break and continue leave the scope on their own edges
static int byJump(int n) {
	int total = 0, i;
	for(i = 0; i < n; i++) {
		int x[i % 500 + 1];
		x[0] = i;
		p = x;
		if(i % 3 == 0)
			continue;
		if(i == n - 1)
			break;
		total++;
	}
	return total;
}

int main(void) {
	byBlock();
	byGoto();
	byGotoInner();
	// 200000 iterations, 66667 of them skipped by continue, the last one broken
	if(byJump(200000) != 133332)
		return 1;
	return 0;
}
