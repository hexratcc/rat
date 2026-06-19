// expect: 36
// Nested calls: square(double(3)) = (3+3)^2 = 36.
int dbl(int x) {
	return x + x;
}

int square(int x) {
	return x * x;
}

int main(void) {
	return square(dbl(3));
}
