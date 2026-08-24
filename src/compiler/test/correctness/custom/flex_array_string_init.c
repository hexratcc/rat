// expect: 0
// passes:

struct a {
	char c;
	char p[];
};

struct a s = {'o', "wx"};
struct a d = {.c = '9', .p = "qrst"};
struct a b = {'e', {'g', 'h'}};

int main(void) {
	if(s.c != 'o' || s.p[0] != 'w' || s.p[1] != 'x' || s.p[2] != 0)
		return 1;
	if(d.c != '9' || d.p[0] != 'q' || d.p[3] != 't' || d.p[4] != 0)
		return 2;
	if(b.c != 'e' || b.p[0] != 'g' || b.p[1] != 'h')
		return 3;
	return 0;
}
