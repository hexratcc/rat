// expect: 0
// passes:

long ident(long x);
int cfg(int op, ...);

static long wrap(long x) { return ident(x); }

static const int cell = 3;
static long seenCell;

static int setdef(void) { return cfg(4, &cell); }

long ident(long x) { return x; }

int cfg(int op, ...) {
	__builtin_va_list ap;
	__builtin_va_start(ap, op);
	const int* p = __builtin_va_arg(ap, const int*);
	seenCell = *p;
	__builtin_va_end(ap);
	return op;
}

int main(void) {
	if(wrap(7) != 7)
		return 1;
	if(setdef() != 4) // rdi held 0, not 4, before the fix
		return 2;
	if(seenCell != 3)
		return 3;
	return 0;
}
