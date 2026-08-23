// expect: 0

volatile int i1 = 2000000000;
volatile int i2 = -2000000000;
volatile int imin = -2147483647 - 1;
volatile unsigned u1 = 2654435761u;
volatile unsigned u2 = 20251u;
volatile unsigned u3 = 131071u;

volatile int addWrap = -294967296;
volatile int subWrap = 294967296;
volatile unsigned mulWrap = 2654318821u;
volatile unsigned subMul = 116940u;

int main(void) {
	if(i1 + i1 != addWrap)
		return 1;
	if(i2 - i1 != subWrap)
		return 2;
	if(u2 * u3 != mulWrap)
		return 3;
	if(u1 - u2 * u3 != subMul)
		return 4;
	if(-imin != imin)
		return 5;
	if(i1 * 3 != 1705032704)
		return 6;
	if(i1 + i1 < 0 == 0)
		return 7;
	return 0;
}
