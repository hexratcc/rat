// expect: 0
// output:
//| sadd     1 ffffffff80000000
//| saddl    1 8000000000000000
//| saddll   1 8000000000000000
//| uadd     1 0000000000000000
//| uaddl    1 0000000000000000
//| uaddll   1 0000000000000000
//| ssub     1 000000007fffffff
//| ssubl    1 7fffffffffffffff
//| ssubll   1 7fffffffffffffff
//| usub     1 00000000ffffffff
//| usubl    1 ffffffffffffffff
//| usubll   1 ffffffffffffffff
//| smul     1 ffffffff80000000
//| smull    1 8000000000000000
//| smulll   1 8000000000000000
//| umul     1 0000000000000000
//| umull    1 0000000000000000
//| umulll   1 0000000000000000
//| nosadd   0 000000007ffffffe
//| nouaddll 0 fffffffffffffffe
//| nosmul   0 000000003fffffff
//| noumul   0 00000000fffe0001
#include <stdio.h>

volatile int i_max = 2147483647, i_min = -2147483647 - 1, i_one = 1, i_two = 2;
volatile unsigned u_max = 4294967295u, u_one = 1u, u_half = 2147483648u, u_65535 = 65535u;
volatile long l_max = 9223372036854775807L, l_min = -9223372036854775807L - 1, l_one = 1;
volatile unsigned long ul_max = 18446744073709551615UL, ul_one = 1, ul_half = 9223372036854775808UL;
volatile long long ll_max = 9223372036854775807LL, ll_min = -9223372036854775807LL - 1;
volatile long long ll_one = 1, ll_two = 2;
volatile unsigned long long ull_max = 18446744073709551615ULL, ull_one = 1, ull_two = 2;
volatile unsigned long long ull_half = 9223372036854775808ULL;

#define SHOW(tag, call, res)                                                                       \
	do {                                                                                             \
		int f = (call);                                                                                \
		printf("%-8s %d %016llx\n", tag, f, (unsigned long long)(res));                                \
	} while(0)

int main(void) {
	int i;
	unsigned u;
	long l;
	unsigned long ul;
	long long ll;
	unsigned long long ull;

	SHOW("sadd", __builtin_sadd_overflow(i_max, i_one, &i), i);
	SHOW("saddl", __builtin_saddl_overflow(l_max, l_one, &l), l);
	SHOW("saddll", __builtin_saddll_overflow(ll_max, ll_one, &ll), ll);
	SHOW("uadd", __builtin_uadd_overflow(u_max, u_one, &u), u);
	SHOW("uaddl", __builtin_uaddl_overflow(ul_max, ul_one, &ul), ul);
	SHOW("uaddll", __builtin_uaddll_overflow(ull_max, ull_one, &ull), ull);

	SHOW("ssub", __builtin_ssub_overflow(i_min, i_one, &i), i);
	SHOW("ssubl", __builtin_ssubl_overflow(l_min, l_one, &l), l);
	SHOW("ssubll", __builtin_ssubll_overflow(ll_min, ll_one, &ll), ll);
	SHOW("usub", __builtin_usub_overflow(0u, u_one, &u), u);
	SHOW("usubl", __builtin_usubl_overflow(0UL, ul_one, &ul), ul);
	SHOW("usubll", __builtin_usubll_overflow(0ULL, ull_one, &ull), ull);

	SHOW("smul", __builtin_smul_overflow(i_min, i_one - i_two, &i), i);
	SHOW("smull", __builtin_smull_overflow(l_min, -l_one, &l), l);
	SHOW("smulll", __builtin_smulll_overflow(ll_min, -ll_one, &ll), ll);
	SHOW("umul", __builtin_umul_overflow(u_half, i_two, &u), u);
	SHOW("umull", __builtin_umull_overflow(ul_half, ull_two, &ul), ul);
	SHOW("umulll", __builtin_umulll_overflow(ull_half, ull_two, &ull), ull);

	// the same forms where nothing overflows
	SHOW("nosadd", __builtin_sadd_overflow(i_max, -i_one, &i), i);
	SHOW("nouaddll", __builtin_uaddll_overflow(ull_max - ull_two, ull_one, &ull), ull);
	SHOW("nosmul", __builtin_smul_overflow(1073741823, i_one, &i), i);
	SHOW("noumul", __builtin_umul_overflow(u_65535, u_65535, &u), u);
	return 0;
}
