// expect: 66
// Store characters into a char array and read one back.
int main(void) {
	char buf[3];
	buf[0] = 'A';
	buf[1] = 'B';
	buf[2] = 'C';
	return buf[1];
}
