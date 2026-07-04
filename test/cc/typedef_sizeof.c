// expect: 12
// Typedef names work in sizeof and cast contexts.
typedef int Int;

int main(void) {
	return sizeof(Int) + sizeof(Int*);
}
