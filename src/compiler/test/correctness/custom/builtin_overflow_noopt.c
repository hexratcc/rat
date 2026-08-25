// expect: 0
// passes:

volatile int i_min = -2147483647 - 1, i_max = 2147483647, i_one = 1, i_neg = -1;
volatile unsigned u_max = 4294967295u, u_one = 1u, u_two = 2u, u_half = 2147483648u;
volatile long long ll_min = -9223372036854775807LL - 1, ll_max = 9223372036854775807LL;
volatile long long ll_one = 1, ll_neg = -1, ll_two = 2, ll_root = 3037000499LL;
volatile unsigned long long ull_max = 18446744073709551615ULL, ull_one = 1ULL;
volatile unsigned long long ull_2p32 = 4294967296ULL, ull_half = 9223372036854775808ULL;
volatile signed char sc_two = 2;
volatile unsigned char uc_max = 255;

static int code;
#define T(cond)                                                                                    \
	do {                                                                                             \
		++code;                                                                                        \
		if(!(cond))                                                                                    \
			return code;                                                                                 \
	} while(0)

int main(void) {
	int i;
	unsigned u;
	signed char sc;
	unsigned short us;
	long long ll;
	unsigned long long ull;

	// 64-bit signed
	T(__builtin_add_overflow(ll_max, ll_one, &ll) == 1 && ll == ll_min);
	T(__builtin_add_overflow(ll_min, ll_neg, &ll) == 1 && ll == ll_max);
	T(__builtin_sub_overflow(ll_min, ll_one, &ll) == 1 && ll == ll_max);
	T(__builtin_sub_overflow(ll_max, ll_neg, &ll) == 1 && ll == ll_min);
	T(__builtin_mul_overflow(ll_min, ll_neg, &ll) == 1 && ll == ll_min);
	T(__builtin_mul_overflow(ll_min, ll_two, &ll) == 1 && ll == 0);
	T(__builtin_mul_overflow(ll_root, ll_root, &ll) == 0 && ll == 9223372030926249001LL);
	T(__builtin_add_overflow(ll_max, ll_neg, &ll) == 0 && ll == 9223372036854775806LL);

	// 64-bit unsigned
	T(__builtin_add_overflow(ull_max, ull_one, &ull) == 1 && ull == 0);
	T(__builtin_sub_overflow(0ULL, ull_one, &ull) == 1 && ull == ull_max);
	T(__builtin_mul_overflow(ull_2p32, ull_2p32, &ull) == 1 && ull == 0);
	T(__builtin_mul_overflow(ull_half, ull_one, &ull) == 0 && ull == ull_half);
	T(__builtin_mul_overflow(ull_max, ull_max, &ull) == 1 && ull == 1);
	T(__builtin_add_overflow(ull_half, ull_half, &ull) == 1 && ull == 0);

	// mixed operand signedness and a narrower or wider result
	T(__builtin_mul_overflow(u_two, i_min / 2, &i) == 0 && i == i_min);
	T(__builtin_mul_overflow(i_neg, ull_one, &ll) == 0 && ll == -1);
	T(__builtin_mul_overflow(i_neg, ull_one, &ull) == 1 && ull == ull_max);
	T(__builtin_mul_overflow(ll_min, sc_two, &ull) == 1 && ull == 0);
	T(__builtin_add_overflow(ll_neg, ull_max, &ull) == 0 && ull == 18446744073709551614ULL);
	T(__builtin_sub_overflow(ull_max, i_neg, &ull) == 1 && ull == 0);
	T(__builtin_add_overflow(uc_max, i_neg, &ull) == 0 && ull == 254);
	T(__builtin_add_overflow(i_neg, uc_max, &sc) == 1 && sc == -2);
	T(__builtin_mul_overflow(u_max, u_max, &ull) == 0 && ull == 18446744065119617025ULL);
	T(__builtin_mul_overflow(u_half, u_two, &u) == 1 && u == 0);
	T(__builtin_mul_overflow(i_max, i_max, &us) == 1 && us == 1);
	T(__builtin_add_overflow(i_max, i_one, &ll) == 0 && ll == 2147483648LL);

	// the _p forms take the result type from the third argument and store nothing
	T(__builtin_mul_overflow_p(ll_min, ll_neg, (long long)0) == 1);
	T(__builtin_mul_overflow_p(ll_min, ll_neg, (unsigned long long)0) == 0);
	T(__builtin_add_overflow_p(ull_max, ull_one, (unsigned long long)0) == 1);
	T(__builtin_sub_overflow_p(0ULL, ull_one, (long long)0) == 0);
	return 0;
}
