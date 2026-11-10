#include "debug.h"

struct Point {
	int x;
	double y;
	unsigned flag : 3;
	char name[8];
};

int counter = 7;

__attribute__((noinline)) int leaf(int a) { return a * 3 + 1; }

__attribute__((noinline)) int deep(int n, int acc) {
	if(n == 0)
		return leaf(acc);
	int t = square(n) + TWICE(acc);
	return deep(n - 1, t) + t;
}

__attribute__((noinline)) double scale(struct Point* p, int k) {
	static int calls;
	double r = p->y * k;
	calls++;
	return r;
}

__attribute__((noinline)) int span(struct Point q, int bias) {
	int w = q.x + bias;
	counter += w;
	return w + q.flag;
}

int main(void) {
	struct Point p = {1, 2.5, 5, "hi"};
	int arr[3] = {4, 5, 6};
	char ch = 'a';
	int r = deep(3, 1);
	double d = scale(&p, arr[1]);
	r += span(p, 2);
	return (r + (int)d + ch + counter) & 1;
}

typedef enum Color { Red, Green = 5, Blue = -2 } Color;
typedef struct {
	Color c;
	const char* s;
} Tagged;
Tagged tagged = {Green, "x"};
