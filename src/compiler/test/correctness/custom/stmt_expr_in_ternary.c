// expect: 5

int main(void) {
	int c = 1;
	int a = c ? ({ int y = 5; y; }) : 0;
	return a;
}
