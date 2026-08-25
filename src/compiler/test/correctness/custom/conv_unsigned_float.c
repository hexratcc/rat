// expect: 0

volatile unsigned char u8;
volatile unsigned short u16;
volatile unsigned int u32;
volatile unsigned long long u64;
volatile int s32;
volatile long long s64;
volatile double d;
volatile float f;

int main(void) {
	// unsigned -> floating
	u8 = 255;
	if((double)u8 != 255.0)
		return 1;
	u16 = 65535;
	if((double)u16 != 65535.0)
		return 2;
	u32 = 4294967295u;
	if((double)u32 != 4294967295.0)
		return 3;
	if((float)u32 != 4294967296.0f) // 32 ones round up to 2^32 in a float
		return 4;
	u64 = 1ull;
	if((double)u64 != 1.0)
		return 5;
	u64 = 9223372036854775808ull; // 2^63, the first value cvtsi2sd gets wrong
	if((double)u64 != 9223372036854775808.0)
		return 6;
	u64 = 10000000000000000000ull;
	if((double)u64 != 10000000000000000000.0)
		return 7;
	u64 = 18446744073709551615ull;
	if((double)u64 != 18446744073709551616.0) // rounds to 2^64
		return 8;
	if((float)u64 != 18446744073709551616.0f)
		return 9;

	// signed -> floating must be unchanged
	s32 = -1;
	if((double)s32 != -1.0)
		return 10;
	s64 = -9223372036854775807LL - 1;
	if((double)s64 != -9223372036854775808.0)
		return 11;

	// floating -> unsigned
	d = 255.9;
	if((unsigned char)d != 255)
		return 12;
	d = 65535.5;
	if((unsigned short)d != 65535)
		return 13;
	d = 4294967295.0;
	if((unsigned int)d != 4294967295u)
		return 14;
	d = 1.5;
	if((unsigned long long)d != 1ull)
		return 15;
	d = 9223372036854775808.0; // 2^63, where the bias fixup starts
	if((unsigned long long)d != 9223372036854775808ull)
		return 16;
	d = 10000000000000000000.0;
	if((unsigned long long)d != 10000000000000000000ull)
		return 17;
	d = 18446744073709549568.0; // the largest double below 2^64
	if((unsigned long long)d != 18446744073709549568ull)
		return 18;
	f = 9223372036854775808.0f;
	if((unsigned long long)f != 9223372036854775808ull)
		return 19;

	// floating -> signed must be unchanged
	d = -3.75;
	if((int)d != -3)
		return 20;
	if((long long)d != -3)
		return 21;

	// the constant-folded forms
	if((double)(unsigned char)255 != 255.0)
		return 22;
	if((double)(unsigned int)4294967295u != 4294967295.0)
		return 23;
	if((double)(unsigned long long)18446744073709551615ull != 18446744073709551616.0)
		return 24;
	return 0;
}
