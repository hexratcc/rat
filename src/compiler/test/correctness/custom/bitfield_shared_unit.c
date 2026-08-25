// expect: 0
struct S {
	short a;
	char b;
	int c : 2;
	int d : 3;
	int e : 3;
};

int main(void) {
	struct S x = {1, 2, 3, 4, 5};
	if(sizeof(struct S) != 4)
		return 1;
	if(x.a != 1)
		return 2;
	if(x.b != 2)
		return 3;
	if(x.c != -1)
		return 4;
	if(x.d != -4)
		return 5;
	if(x.e != -3)
		return 6;
	if(sizeof(struct { char a; int b : 5; }) != 4)
		return 7;
	if(sizeof(struct { int a : 3; int : 0; int c : 5; }) != 8)
		return 8;
	return 0;
}
