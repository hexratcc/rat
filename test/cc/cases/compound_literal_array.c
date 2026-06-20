// expect: 15
// C99 array compound literals, indexed directly.
int main(void) {
	return (int[]){4, 5, 6}[1] + (int[]){10, 20}[0];
}
