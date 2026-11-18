// expect: 0
struct S {
	int pad;
	int m[3];
};
int g[4];
char* pastConst = (char*)(&g + 1);

int main(void) {
	int a[4];
	struct S s;
	a[2] = 7;
	// &array points to the whole array
	if((char*)(&a + 1) - (char*)&a != sizeof a)
		return 1;
	if((*&a)[2] != 7)
		return 2;
	if((char*)(&s.m + 1) - (char*)&s.m != sizeof s.m)
		return 3;
	if(pastConst != (char*)g + sizeof g)
		return 4;
	return 0;
}
