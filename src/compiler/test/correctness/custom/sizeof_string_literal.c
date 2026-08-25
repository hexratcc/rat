// expect: 0

struct S {
	char b[8];
};

char g[] = "abc";
struct S gs = {"hi"};

int main(void) {
	int a[10];

	if(sizeof("abcdef") != 7)
		return 1;
	if(sizeof("") != 1)
		return 2;
	if(sizeof(L"ab") != 3 * sizeof(L"a"[0]))
		return 3;
	if(sizeof("message") - 1 != 7)
		return 4;
	if(L"ab"[2] != 0) // the terminator is one element wide, not one byte
		return 5;

	// the decayed forms stay pointer-sized
	if(sizeof("abc" + 0) != sizeof(char*))
		return 6;
	if(sizeof((char*)"abc") != sizeof(char*))
		return 7;
	if(sizeof("abc"[0]) != 1)
		return 8;

	// named arrays and array members keep their sizes
	if(sizeof(g) != 4)
		return 9;
	if(sizeof(gs.b) != 8)
		return 10;
	if(sizeof(a) != 10 * sizeof(int))
		return 11;
	return 0;
}
