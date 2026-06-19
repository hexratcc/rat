// expect: 30
// 'static' / 'inline' on functions are accepted and ignored.
static int helper(int a) { return a * 3; }
inline int main(void) {
	return helper(10);
}
