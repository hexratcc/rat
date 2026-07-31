// expect: 55
// variable-index double array: float load becomes an indexed movsd (SIB
// base+index*8); the float store lowers to lea then movsd
int main(void) {
	double a[10];
	long i = 0;
	while (i < 10) {
		a[i] = (double)(i + 1);
		i = i + 1;
	}
	double sum = 0.0;
	long j = 0;
	while (j < 10) {
		sum = sum + a[j];
		j = j + 1;
	}
	return (int)sum; // 1+2+...+10 = 55
}
