// expect: 50
// Compound assignment reads and writes the global in place.
int total = 5;

int main(void) {
	total += 45;
	return total;
}
