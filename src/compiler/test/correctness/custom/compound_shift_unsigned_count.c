// expect: 0

// a compound shift promotes each operand on its own, the count's type does not matter
int main(void) {
	int x = -8;
	x >>= 1u;
	return x != -4;
}
