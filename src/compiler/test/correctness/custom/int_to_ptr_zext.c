// expect: 0
// output:
//| 0x80000000
//| 0xffffffff80000000
//| 0x80000000
//| 0x1
//| 0xffffffffffff8000

#include <stdio.h>

unsigned getu(int k) {
	return 0x80000000u + (unsigned)k;
}
int geti(int k) {
	return (int)0x80000000 + k;
}
_Bool getb(int k) {
	return k == 0;
}
short gets(int k) {
	return (short)(-32768 + k);
}

int main(void) {
	unsigned u = getu(0);
	printf("%p\n", (void *)u);
	int i = geti(0);
	printf("%p\n", (void *)i);
	printf("%p\n", (void *)0x80000000u);
	printf("%p\n", (void *)getb(0));
	short s = gets(0);
	printf("%p\n", (void *)s);
	return 0;
}
