// expect: 0

// a qualifier on a pointer inside a parenthesized declarator is kept

typedef int (*Handler)(void);
typedef int (*Row)[3];

int (*const handler)(void) = 0;
int (*const row)[3] = 0;
int* (*const nested) = 0;

int main(void) {
	if(_Generic(&handler, const Handler*: 0, default: 1))
		return 1;
	if(_Generic(&row, const Row*: 0, default: 1))
		return 2;
	if(_Generic(&nested, int** const*: 0, default: 1))
		return 3;
	return _Generic(&handler, Handler*: 4, default: 0);
}
