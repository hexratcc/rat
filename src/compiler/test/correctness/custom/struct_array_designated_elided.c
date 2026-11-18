// expect: 24
// A designator inside a brace-elided struct array starts a new element.
struct P {
	int x, y;
};

struct P b[] = {[0] = 1, 2, [1] = 3, 4};

int main(void) {
	return sizeof b / sizeof b[0] * 10 + b[1].y;
}
