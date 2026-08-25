// expect: 0
// passes:

volatile double negz = -0.0;
volatile double one = 1.0;

int main(void) {
	if(__builtin_copysign(one, negz) >= 0.0)
		return 1;
	if(__builtin_copysign(one, one) <= 0.0)
		return 2;
	if(__builtin_fabs(negz) != 0.0)
		return 3;
	if(__builtin_sqrt(4.0) != 2.0)
		return 4;
	if(__builtin_strlen("abcd") != 4)
		return 5;
	return 0;
}
