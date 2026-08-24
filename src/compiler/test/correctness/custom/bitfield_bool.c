// expect: 0
// passes:

struct S {
	_Bool a : 1;
	_Bool b : 1;
	_Bool c : 1;
};

struct S s;

int main(void) {
	_Bool t = 1;
	s.a = t;
	if(!s.a)
		return 1;
	s.c = t;
	if(!s.c || s.b)
		return 2;
	s.a = 0;
	if(s.a || !s.c)
		return 3;
	return 0;
}
