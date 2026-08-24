// expect: 0
// passes:

typedef __SIZE_TYPE__ Size_t;

#define bufsize ((1L << (8 * sizeof(Size_t) - 2)) - 256)

struct huge {
	short buf[bufsize];
	int a;
	int b;
};

union huge_u {
	int a;
	char buf[bufsize];
};

struct small {
	char pad[1L << 33];
	int tail;
};

int main(void) {
	if(sizeof(union huge_u) != sizeof(char) * bufsize)
		return 1;
	if(sizeof(struct huge) != sizeof(short) * bufsize + 2 * sizeof(int))
		return 2;
	if((Size_t)(&((struct huge*)0)->a) != sizeof(short) * bufsize)
		return 3;
	if(sizeof(struct small) != (1L << 33) + 4)
		return 4;
	if((Size_t)(&((struct small*)0)->tail) != (1L << 33))
		return 5;
	if(sizeof(int[1L << 32]) != (1L << 34))
		return 6;
	return 0;
}
