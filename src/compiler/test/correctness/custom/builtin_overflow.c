// expect: 0

volatile signed char sc_min = -128, sc_max = 127, sc_two = 2;
volatile unsigned char uc_max = 255, uc_16 = 16;
volatile short sh_min = -32768, sh_max = 32767, sh_two = 2;
volatile unsigned short us_max = 65535, us_256 = 256;
volatile int i_min = -2147483647 - 1, i_max = 2147483647, i_one = 1, i_neg = -1;
volatile int i_64k = 65536, i_half = -1073741824;
volatile unsigned u_max = 4294967295u, u_half = 2147483648u, u_one = 1u, u_two = 2u;
volatile unsigned u_64k = 65536u, u_65535 = 65535u;
volatile long long ll_min = -9223372036854775807LL - 1, ll_max = 9223372036854775807LL;
volatile long long ll_one = 1, ll_neg = -1, ll_root = 3037000499LL, ll_root1 = 3037000500LL;
volatile unsigned long long ull_max = 18446744073709551615ULL, ull_one = 1ULL;
volatile unsigned long long ull_2p32 = 4294967296ULL, ull_4g1 = 4294967295ULL;

static int code;
#define T(cond)                                                                                    \
	do {                                                                                             \
		++code;                                                                                        \
		if(!(cond))                                                                                    \
			return code;                                                                                 \
	} while(0)

int main(void) {
	signed char sc;
	unsigned char uc;
	short sh;
	unsigned short us;
	int i;
	unsigned u;
	long long ll;
	unsigned long long ull;

	// signed char
	T(__builtin_add_overflow(sc_max, sc_max, &sc) == 1 && sc == -2);
	T(__builtin_sub_overflow(sc_min, sc_max, &sc) == 1 && sc == 1);
	T(__builtin_mul_overflow(sc_max, sc_two, &sc) == 1 && sc == -2);
	T(__builtin_add_overflow(sc_min, sc_max, &sc) == 0 && sc == -1);

	// unsigned char
	T(__builtin_add_overflow(uc_max, uc_max, &uc) == 1 && uc == 254);
	T(__builtin_sub_overflow(uc_16, uc_max, &uc) == 1 && uc == 17);
	T(__builtin_mul_overflow(uc_16, uc_16, &uc) == 1 && uc == 0);
	T(__builtin_mul_overflow(uc_16, uc_max, &uc) == 1 && uc == 240);

	// short
	T(__builtin_add_overflow(sh_max, sh_max, &sh) == 1 && sh == -2);
	T(__builtin_sub_overflow(sh_min, sh_max, &sh) == 1 && sh == 1);
	T(__builtin_mul_overflow(sh_max, sh_two, &sh) == 1 && sh == -2);
	T(__builtin_mul_overflow(sh_min, sh_two, &sh) == 1 && sh == 0);

	// unsigned short
	T(__builtin_add_overflow(us_max, us_max, &us) == 1 && us == 65534);
	T(__builtin_sub_overflow(us_256, us_max, &us) == 1 && us == 257);
	T(__builtin_mul_overflow(us_256, us_256, &us) == 1 && us == 0);
	T(__builtin_mul_overflow(us_256, us_max, &us) == 1 && us == 65280);

	// int, at both ends of the range
	T(__builtin_add_overflow(i_max, i_one, &i) == 1 && i == i_min);
	T(__builtin_add_overflow(i_max, 0, &i) == 0 && i == i_max);
	T(__builtin_sub_overflow(i_min, i_one, &i) == 1 && i == i_max);
	T(__builtin_sub_overflow(i_min, 0, &i) == 0 && i == i_min);
	T(__builtin_mul_overflow(i_min, i_neg, &i) == 1 && i == i_min);
	T(__builtin_mul_overflow(i_64k, i_64k, &i) == 1 && i == 0);
	T(__builtin_mul_overflow(i_half, sc_two, &i) == 0 && i == i_min);

	// unsigned
	T(__builtin_add_overflow(u_max, u_one, &u) == 1 && u == 0);
	T(__builtin_sub_overflow(0u, u_one, &u) == 1 && u == u_max);
	T(__builtin_mul_overflow(u_half, u_two, &u) == 1 && u == 0);
	T(__builtin_mul_overflow(u_64k, u_64k, &u) == 1 && u == 0);
	T(__builtin_mul_overflow(u_65535, u_65535, &u) == 0 && u == 4294836225u);

	// long long
	T(__builtin_add_overflow(ll_max, ll_one, &ll) == 1 && ll == ll_min);
	T(__builtin_add_overflow(ll_max, ll_neg, &ll) == 0 && ll == 9223372036854775806LL);
	T(__builtin_sub_overflow(ll_min, ll_one, &ll) == 1 && ll == ll_max);
	T(__builtin_mul_overflow(ll_min, ll_neg, &ll) == 1 && ll == ll_min);
	T(__builtin_mul_overflow(ll_root, ll_root, &ll) == 0 && ll == 9223372030926249001LL);
	T(__builtin_mul_overflow(ll_root1, ll_root1, &ll) == 1 && ll == -9223372036709301616LL);

	// unsigned long long
	T(__builtin_add_overflow(ull_max, ull_one, &ull) == 1 && ull == 0);
	T(__builtin_sub_overflow(0ULL, ull_one, &ull) == 1 && ull == ull_max);
	T(__builtin_mul_overflow(ull_2p32, ull_2p32, &ull) == 1 && ull == 0);
	T(__builtin_mul_overflow(ull_4g1, ull_4g1, &ull) == 0 && ull == 18446744065119617025ULL);
	T(__builtin_mul_overflow(ull_max, ull_max, &ull) == 1 && ull == 1);

	// operand types that differ from each other and from the result type
	T(__builtin_mul_overflow(u_two, i_half, &i) == 0 && i == i_min);
	T(__builtin_mul_overflow(i_neg, ll_one, &u) == 1 && u == u_max);
	T(__builtin_add_overflow(uc_max, i_neg, &ull) == 0 && ull == 254);
	T(__builtin_add_overflow(i_neg, uc_max, &sc) == 1 && sc == -2);
	T(__builtin_sub_overflow(ull_max, i_neg, &ull) == 1 && ull == 0);
	T(__builtin_mul_overflow(ll_min, sc_two, &ull) == 1 && ull == 0);
	T(__builtin_mul_overflow(ull_one + 2, ll_neg, &ll) == 0 && ll == -3);
	T(__builtin_add_overflow(ll_neg, ull_max, &ull) == 0 && ull == 18446744073709551614ULL);
	T(__builtin_add_overflow(8719476735LL, 0, &i) == 1 && i == 129542143);
	T(__builtin_mul_overflow(i_min, i_neg, &ll) == 0 && ll == 2147483648LL);
	T(__builtin_mul_overflow(u_max, u_max, &ull) == 0 && ull == 18446744065119617025ULL);
	T(__builtin_mul_overflow(u_max, u_max, &ll) == 1 && ll == -8589934591LL);

	// the _p forms return the same flag and store nothing
	T(__builtin_add_overflow_p(i_max, i_one, (int)0) == 1);
	T(__builtin_add_overflow_p(i_max, i_one, (long long)0) == 0);
	T(__builtin_mul_overflow_p(4, (unsigned char)254, 0) == 0);
	T(__builtin_mul_overflow_p(ll_min, ll_neg, (long long)0) == 1);
	T(__builtin_sub_overflow_p(0u, u_one, (unsigned)0) == 1);
	T(__builtin_sub_overflow_p(0u, u_one, (int)0) == 0);
	T(__builtin_sub_overflow_p(0u, u_one, (long long)0) == 0);

	// the third argument of a _p form is evaluated for its side effects only
	i = 0;
	T(__builtin_add_overflow_p(i_max, i_one, i++) == 1);
	T(i == 1);
	return 0;
}
