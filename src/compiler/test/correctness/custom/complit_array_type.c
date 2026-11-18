// expect: 0
// Array compound literals take their size from the array type, also through a typedef.
typedef int A[3];

int main(void) {
	int *p = (A){1, 2, 3};
	if(p[0] + p[1] + p[2] != 6)
		return 1;
	if(sizeof((A){1, 2, 3}) != 3 * sizeof(int))
		return 2;
	if(sizeof((int[3]){1, 2, 3}) != 3 * sizeof(int))
		return 3;
	return 0;
}
