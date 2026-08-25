// expect: 0
// passes:

struct S {
	unsigned int u7 : 7;
	signed int s11 : 11;
	unsigned long int u31 : 31;
	unsigned long int u32 : 32;
	signed long int s24 : 24;
};

struct S s;

int main(void) {
	int i = -13;

	s.u7 = 61;
	// u7 promotes to int, so the remainder is the signed one
	if(i % s.u7 != -13 % 61)
		return 1;
	// the cast forces the full unsigned int type back
	if(i % (unsigned int)s.u7 != -13U % 61)
		return 2;

	// the result of the assignment is the stored value, wrapped to 11 bits
	if((s.s11 = 1081) == 1081)
		return 3;
	if(s.s11 != -967)
		return 4;

	// 31 unsigned bits still fit in int, 32 do not
	if((s.u31 - 2) >= 0)
		return 5;
	if((s.u32 - 2) < 0)
		return 6;

	// a 24-bit signed field sign-extends on load
	s.s24 = 0xFEFEFEFE;
	if(s.s24 != (int)0xFFFEFEFE)
		return 7;
	return 0;
}
