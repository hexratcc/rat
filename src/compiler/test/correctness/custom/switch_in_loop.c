// expect: 8
// 'continue' inside a switch skips to the loop step; 'break' only exits the
// switch. Sum over i in 0..4 of i, skipping i==2 (continue): 0+1+3+4 == 8.
int main(void) {
	int sum = 0;
	int i;
	for (i = 0; i < 5; i++) {
		switch (i) {
		case 2:
			continue;
		case 4:
			break;
		}
		sum = sum + i;
	}
	return sum;
}
