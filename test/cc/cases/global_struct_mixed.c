// expect: 1065
// A file-scope struct with mixed field widths respects offsets/padding.
struct P { char c; int n; };
struct P g = {65, 1000};
int main(void) {
	return g.c + g.n;
}
