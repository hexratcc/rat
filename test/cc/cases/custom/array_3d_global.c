// expect: 1
// A file-scope 2D array plus a local 3D array; rows decay to pointers.
int g[2][2] = {{10, 20}, {30, 40}};
int rowsum(int *r, int n) {
	int s = 0;
	int i = 0;
	for (i = 0; i < n; i = i + 1)
		s = s + r[i];
	return s;
}
int main(void) {
	int t[2][2][2] = {{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}};
	int total = g[0][0] + g[0][1] + g[1][0] + g[1][1]; // 100
	return total == 100 && t[1][0][1] == 6 && rowsum(g[1], 2) == 70;
}
