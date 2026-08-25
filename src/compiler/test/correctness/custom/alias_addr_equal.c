// expect: 0

int a = 1;
extern int b __attribute__((alias("a")));
int c = 1;
extern int d __attribute__((alias("c")));

int* pa = &a;
int* pb = &b;

int main(int argc, char** argv) {
	(void)argv;
	if(&a != &b)
		return 1;
	if(&c != &d)
		return 2;
	if(pa != pb)
		return 3;

	int* p;
	int* q;
	if(argc) {
		p = &a;
		q = &b;
	} else {
		p = &c;
		q = &d;
	}
	*p = 1;
	*q = 2;
	if(*p != 2)
		return 4;
	return 0;
}
