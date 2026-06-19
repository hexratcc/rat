// expect: 100
// Sum an array in a loop using pointer arithmetic on the decayed base.
int main(void) {
	int a[5];
	int i = 0;
	while (i < 5) {
		a[i] = i * 10;
		i = i + 1;
	}
	int sum = 0;
	int *p = a;
	while (p < a + 5) {
		sum = sum + *p;
		p = p + 1;
	}
	return sum;
}
