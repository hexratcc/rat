// expect: 12
// 5 > 3 picks 10; 2 < 1 false picks 2; 10 + 2.
int main(void) {
	return (5 > 3 ? 10 : 20) + (2 < 1 ? 1 : 2);
}
