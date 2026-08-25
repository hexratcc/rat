// expect: 0
char a[7] = "foobar";
char* fromConst = &a - 3;
int a2[3][4];
int (*fromConst2)[4] = &a2[0] - 1;

int main(void) {
	char* fromRuntime = &a - 3;
	if(fromConst != fromRuntime)
		return 1;
	if(fromConst != a - 3)
		return 2;
	// a subarray keeps its own element size
	if((char*)fromConst2 != (char*)a2 - 16)
		return 3;
	return 0;
}
