// expect: 23
// A global accumulates across calls; ++ and & operate on it directly.
int counter = 10;
unsigned int mask = 255;

int add(int x) {
	counter = counter + x;
	return counter;
}

int main(void) {
	add(5);
	add(7);
	counter++;
	return counter & mask;
}
