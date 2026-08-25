// expect: 65
// Narrowing to char truncates to 8 bits: 321 & 0xFF == 65.
int main(void) {
	int x = 321;
	return (char)x;
}
