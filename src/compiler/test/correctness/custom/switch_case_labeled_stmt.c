// expect: 0

volatile int zero = 0;

int guarded(int stage) {
	int r = 0;
	switch(stage) {
	case 0:
		r = 1;
		if(stage == 1)
		case 1: {
			r += 10;
		}
			r += 100;
		break;
	default:
		r = -1;
	}
	return r;
}

// case labels chained onto one statement, and a label ending its block
int chained(int k) {
	int r = 0;
	switch(k) {
	case 1:
	case 2:
		r = 20;
		break;
	case 3:
		r = 30;
	}
	return r;
}

// Duff's device: a case label in the middle of a loop body
int duff(int n) {
	int r = 0;
	int i = n;
	switch(n % 4) {
	case 0:
		do {
			++r;
		case 3:
			++r;
		case 2:
			++r;
		case 1:
			++r;
		} while((i -= 4) > 0);
	}
	return r;
}

// a braceless switch body that is a single labeled statement
int braceless(int k) {
	int r = 7;
	switch(k)
	case 5:
		r = 50;
	return r;
}

int main(void) {
	if(guarded(zero) != 101)
		return 1;
	if(guarded(zero + 1) != 110)
		return 2;
	if(guarded(zero + 9) != -1)
		return 3;
	if(chained(zero + 1) != 20)
		return 4;
	if(chained(zero + 2) != 20)
		return 5;
	if(chained(zero + 3) != 30)
		return 6;
	if(chained(zero + 4) != 0)
		return 7;
	if(duff(zero + 8) != 8)
		return 8;
	if(duff(zero + 7) != 7)
		return 9;
	if(braceless(zero + 5) != 50)
		return 10;
	if(braceless(zero + 6) != 7)
		return 11;
	return 0;
}
