// expect: 0
// passes:
int v = 3;
int a[3] = {7, 8, 9};

static int f(void) {
	int v = 4;
	int a[1] = {0};
	{
		extern int v;
		extern int a[];
		if(v != 3)
			return 1;
		if(a[2] != 9)
			return 2;
		v = 5;
	}
	if(v != 4) // the local is untouched
		return 3;
	return 0;
}

int main(void) {
	int r = f();
	if(r)
		return r;
	if(v != 5) // the global is the one that was written
		return 4;
	return 0;
}
