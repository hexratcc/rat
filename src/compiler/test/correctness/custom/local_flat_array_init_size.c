// expect: 0
// output:
//| 16 16
//| 16 16
//| 16 16
//| 24 24
//| 24 24
//| 16 16
//| 16 16
//| 32 32

#include <stdio.h>

struct P {
	int x, y;
};
struct Q {
	int x;
	int a[2];
};

struct P g1[] = {1, 2, 3, 4};
struct P g2[] = {{1, 2}, {3, 4}};
struct P g3[] = {{1, 2}, 3, 4};
struct Q g4[] = {1, 2, 3, 4, 5, 6};
int g5[][2] = {1, 2, 3, 4, 5, 6};
int g6[][2] = {{1, 2}, {3, 4}};
struct P g7[] = {1, 2, 3};
struct P g8[][2] = {1, 2, 3, 4, 5, 6, 7, 8};

int main(void) {
	struct P a1[] = {1, 2, 3, 4};
	struct P a2[] = {{1, 2}, {3, 4}};
	struct P a3[] = {{1, 2}, 3, 4};
	struct Q a4[] = {1, 2, 3, 4, 5, 6};
	int a5[][2] = {1, 2, 3, 4, 5, 6};
	int a6[][2] = {{1, 2}, {3, 4}};
	struct P a7[] = {1, 2, 3};
	struct P a8[][2] = {1, 2, 3, 4, 5, 6, 7, 8};
	printf("%d %d\n", (int)sizeof g1, (int)sizeof a1);
	printf("%d %d\n", (int)sizeof g2, (int)sizeof a2);
	printf("%d %d\n", (int)sizeof g3, (int)sizeof a3);
	printf("%d %d\n", (int)sizeof g4, (int)sizeof a4);
	printf("%d %d\n", (int)sizeof g5, (int)sizeof a5);
	printf("%d %d\n", (int)sizeof g6, (int)sizeof a6);
	printf("%d %d\n", (int)sizeof g7, (int)sizeof a7);
	printf("%d %d\n", (int)sizeof g8, (int)sizeof a8);
	if(a1[1].y != 4 || a4[1].a[1] != 6 || a5[2][1] != 6 || a8[1][1].y != 8)
		return 1;
	return 0;
}
