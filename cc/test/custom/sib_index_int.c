// expect: 140
// variable-index int array store then load. The store lowers to lea+mov and
// the load to a single indexed movsxd (SIB base+index*4)
int main(void) {
	int a[8];
	long i = 0;
	while (i < 8) {
		a[i] = (int)(i * i);
		i = i + 1;
	}
	int sum = 0;
	long j = 0;
	while (j < 8) {
		sum = sum + a[j];
		j = j + 1;
	}
	return sum; // 0+1+4+9+16+25+36+49 = 140
}
