// expect: 0
int g = 7;

static int readg(void) { return g; }

int main(void) {
	extern int g;
	if(g != 7)
		return 1;
	g = 9;
	if(readg() != 9)
		return 2;
	return 0;
}
