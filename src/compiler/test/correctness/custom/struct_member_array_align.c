// expect: 0
// output:
//| 28 4 4
//| 18 2 2
//| 40 8 8
//| 24 4
//| 60 4

#include <stdio.h>

struct S {
	char c;
	int a[2][3];
};
struct T {
	char c;
	short m[2][2][2];
};
struct U {
	char c;
	double d[2][2];
};
union V {
	char c;
	int a[2][3];
};
struct W {
	char c;
	struct S s[2];
};

int main(void) {
	struct S s;
	printf("%d %d %d\n",
				 (int)sizeof(struct S),
				 (int)_Alignof(struct S),
				 (int)((char*)&s.a[0][0] - (char*)&s));
	printf("%d %d %d\n", (int)sizeof(struct T), (int)_Alignof(struct T), (int)__alignof__(short));
	printf("%d %d %d\n", (int)sizeof(struct U), (int)_Alignof(struct U), (int)__alignof__(double));
	printf("%d %d\n", (int)sizeof(union V), (int)_Alignof(union V));
	printf("%d %d\n", (int)sizeof(struct W), (int)_Alignof(struct W));
	return 0;
}
