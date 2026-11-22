// expect: 15
// host-side:
// narrow int arguments from a host caller, the abi leaves their upper bits undefined
#ifdef RAT_HOST_SIDE
int r_min(int a, int b);
long r_widen(int x);
int r_short_lt(short a, short b);
int r_is_umax(unsigned u);

int g_calls(void) {
	int bits = 0;
	if(r_min(5, -3) == -3)
		bits |= 1;
	if(r_widen(-3) == -3)
		bits |= 2;
	if(r_short_lt(-3, 2))
		bits |= 4;
	if(r_is_umax(~0u))
		bits |= 8;
	return bits;
}
#else
int g_calls(void);

int r_min(int a, int b) { return b < a ? b : a; }
long r_widen(int x) { return x; }
int r_short_lt(short a, short b) { return a < b; }
int r_is_umax(unsigned u) { return u == ~0u; }

int main(void) { return g_calls(); }
#endif
