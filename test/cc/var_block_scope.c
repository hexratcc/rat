// expect: 3
// Inner block shadows the outer x; the outer x is unchanged after the block.
int main(void) {
	int x = 1;
	{
		int x = 2;
		x += 5;
	}
	return x + 2;
}
