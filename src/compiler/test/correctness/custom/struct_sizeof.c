// expect: 8
// sizeof a struct reflects its laid-out size with alignment.
struct Point { int x; int y; };

int main(void) {
	return sizeof(struct Point);
}
