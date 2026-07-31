// expect: 240
// ~((1<<4)-1) clears the low nibble; & 0xFF keeps a byte: 0xF0.
int main(void) {
	return ~((1 << 4) - 1) & 0xFF;
}
