// expect: 140
// short array uses SIB scale 2 (base+index*2)
int main(void) {
	short a[8];
	long i = 0;
	while (i < 8) {
		a[i] = (short)(i * 5);
		i = i + 1;
	}
	int sum = 0;
	long j = 0;
	while (j < 8) {
		sum = sum + a[j];
		j = j + 1;
	}
	return sum; // 5*(0+1+..+7) = 5*28 = 140
}
