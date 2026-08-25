// expect: 0
// passes:

struct S {
	unsigned long long u33 : 33;
	unsigned long long u40 : 40;
	unsigned long long u41 : 41;
};

struct S a = {0x100000, 0x100000, 0x100000};
struct S b = {0x100000000ULL, 0x100000000ULL, 0x100000000ULL};
struct S c = {0x1FFFFFFFFULL, 0, 0};

int main(void) {
	// 2^40 wraps to 0 in 33 and in 40 bits, but survives in 41
	if(a.u33 * a.u33 != 0 || a.u40 * a.u40 != 0)
		return 1;
	if(a.u33 * a.u41 != 0x10000000000ULL)
		return 2;
	// 2^33 wraps to 0 in 33 bits; the wider field wins the common type
	if(b.u33 + b.u33 != 0)
		return 3;
	if(b.u33 + b.u40 != 0x200000000ULL)
		return 4;
	// a plain unsigned long long is wider still, so the shift's own width leaks
	// nothing into the comparison
	if(a.u40 - b.u40 != 0xFF00100000ULL)
		return 5;
	if((b.u40 << 32) != 0)
		return 6;
	// increment and decrement wrap at the field's width too
	if(++c.u33 != 0 || --c.u40 != 0xFFFFFFFFFFULL || c.u41-- != 0)
		return 7;
	return 0;
}
