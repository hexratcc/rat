// expect: 0
// passes:

#define ALIGN 32

typedef struct x {
	int a;
	int b;
} __attribute__((aligned(ALIGN))) X;

typedef struct y {
	X x[4];
	int c;
} Y;

struct member {
	int __attribute__((aligned(8))) a;
};

struct nest {
	char c;
	struct member m;
};

// a trailing marker on the declarator is accepted and does not change the type
struct trail {
	char c;
	int v[2] __attribute__((aligned(4)));
};

Y y[2];
struct nest v;

int main(void) {
	if(sizeof(X) != 32)
		return 1;
	// 4 elements of 32, then int c, rounded up to the 32-byte alignment
	if(sizeof(Y) != 160)
		return 2;
	if(((char*)&y[1] - (char*)&y[0]) & 31)
		return 3;
	if(sizeof(struct member) != 8)
		return 4;
	if((char*)&v.m - (char*)&v != 8)
		return 5;
	if(sizeof(struct trail) != 12)
		return 6;
	return 0;
}
