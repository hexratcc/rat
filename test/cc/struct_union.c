// expect: 65
// Union members overlay the same storage.
union U { int i; char c; };

int main(void) {
	union U u;
	u.i = 65;
	return u.c;
}
