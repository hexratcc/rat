// expect: 0
int main(void) {
	int a[4];
	int (*p)[4] = &a;
	// *p decays inside an expression
	if(sizeof(*p + 1) != sizeof(int*))
		return 1;
	if(sizeof(*p) != sizeof a)
		return 2;
	return 0;
}
