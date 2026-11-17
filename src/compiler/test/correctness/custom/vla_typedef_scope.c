// expect: 0
// A variable length array typedef takes its size where it is declared, not at each use.
int main(void) {
	int n = 3;
	typedef int T[n];
	typedef int (*P)[n];
	{
		int n = 100;
		T shadowed;
		if(sizeof shadowed != 3 * sizeof(int))
			return 1;
	}
	n = 9;
	T reassigned;
	if(sizeof reassigned != 3 * sizeof(int))
		return 2;
	if(sizeof(T) != 3 * sizeof(int))
		return 3;
	P p = &reassigned;
	if((char*)(p + 1) - (char*)p != 3 * sizeof(int))
		return 4;
	T rows[2];
	rows[1][2] = 7;
	if(sizeof rows != 6 * sizeof(int) || ((int*)rows)[5] != 7)
		return 5;
	return 0;
}
