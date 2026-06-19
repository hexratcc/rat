// expect: 30

// object-like macros, including a macro defined in terms of others.
#define WIDTH 5
#define HEIGHT 6
#define AREA (WIDTH * HEIGHT)

int main(void) {
	return AREA;
}
