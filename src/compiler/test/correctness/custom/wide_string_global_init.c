// expect: 0
// a wide string initializer works at file scope, not only inside a function.
// file scope used to demand an 8-bit element type and reject u"..." outright.
unsigned short g[] = u"hi";
unsigned int gl[] = U"hi";
char gc[] = "hi";

int main(void) {
	unsigned short l[] = u"hi";
	if(sizeof g != 6 || g[0] != 104 || g[1] != 105 || g[2] != 0)
		return 1;
	if(sizeof gl != 12 || gl[0] != 104 || gl[2] != 0)
		return 2;
	if(sizeof gc != 3 || gc[0] != 104)
		return 3;
	if(sizeof l != sizeof g || l[0] != g[0] || l[1] != g[1])
		return 4;
	return 0;
}
