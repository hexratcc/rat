// expect: 1
// passes:
int x = 1;

int main(void) {
	switch(x) {
		x = 2;
	lbl:
	case 1:
		return 1;
	}
	return 0;
}
