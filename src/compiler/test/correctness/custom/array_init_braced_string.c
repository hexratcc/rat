// expect: 0
// A string literal in braces initializes a char array like a bare one.
struct Name {
	char text[4];
};

char *lit = (char[]){"abc"};
char unsized[] = {"xyz"};
char sized[5] = {"ab"};
struct Name member = {{"mn"}};

static int same(const char *a, const char *b) {
	while(*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

int main(void) {
	char local[3] = {"ab"};
	char localUnsized[] = {"hello"};
	char *localLit = (char[]){"local"};
	static char stat[] = {"stat"};
	char rows[2][4] = {{"ef"}, "gh"};
	if(!same(lit, "abc") || !same(unsized, "xyz") || !same(sized, "ab") || !same(member.text, "mn"))
		return 1;
	if(sizeof unsized != 4 || sizeof sized != 5 || sizeof localUnsized != 6 || sizeof stat != 5)
		return 2;
	if(sizeof((char[]){"abc"}) != 4)
		return 3;
	if(!same(local, "ab") || !same(localUnsized, "hello") || !same(localLit, "local"))
		return 4;
	if(!same(stat, "stat") || !same(rows[0], "ef") || !same(rows[1], "gh"))
		return 5;
	return 0;
}
