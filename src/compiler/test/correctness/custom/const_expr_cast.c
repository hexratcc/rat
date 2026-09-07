// expect: 0
// output:
//| -56 255 4464 5
//| 2147483647 -56 1 65535
//| 44 4 2

#include <stdio.h>

enum { A = (char)200 };
enum { B = (unsigned char)-1 };
enum { C = (short)70000 };
enum { D = (int)4294967296LL + 5 };
enum { E = (unsigned)-1 / 2 };
enum { F = (long)(char)200 };
enum { G = (_Bool)2 };
enum { H = (unsigned short)-1 };

int arr[(char)200 + 100];
struct BF {
	int f : (char)200 < 0 ? 3 : 5;
};

int main(void) {
	struct BF b;
	b.f = -4;
	printf("%d %d %d %d\n", A, B, C, D);
	printf("%d %d %d %d\n", E, F, G, H);
	printf("%d %d %d\n", (int)(sizeof arr / sizeof arr[0]), (int)sizeof(struct BF), b.f + 6);
	return 0;
}
