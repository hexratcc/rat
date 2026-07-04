// expect: 12
// sizeof of an expression uses its type without evaluating it: a long (8) plus
// the promoted type of a char+char addition (int, 4).
int main(void) {
	long a = 0;
	char b = 0;
	return sizeof a + sizeof(b + b);
}
