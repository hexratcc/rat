// expect: 0
// a wide string initializes a wide array member, at file scope and inside a function

struct W {
	int w[4];
};

struct F {
	int n;
	int w[];
};

struct W g = {L"ab"};
struct F f = {1, L"xy"};

int main(void) {
	struct W l = {L"cd"};
	if(g.w[0] != 'a' || g.w[1] != 'b' || g.w[2] != 0)
		return 1;
	if(l.w[0] != 'c' || l.w[1] != 'd' || l.w[3] != 0)
		return 2;
	if(f.n != 1 || f.w[0] != 'x' || f.w[1] != 'y' || f.w[2] != 0)
		return 3;
	return 0;
}
