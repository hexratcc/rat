// expect: 6
// Designated initializers on a file-scope array with inferred length.
int g[] = { [3] = 4, [1] = 2 };
int main(void) {
	return g[0] + g[1] + g[2] + g[3];
}
