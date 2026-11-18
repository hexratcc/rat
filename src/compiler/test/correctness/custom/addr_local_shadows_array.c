// expect: 0
// &x takes the address of the local when it shadows a global array.
int x[4];

int main(void) {
	int x = 7;
	int *p = &x;
	*p = 9;
	return x != 9;
}
