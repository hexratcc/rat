// expect: 0

int main(void) {
	int x = 7;
	int* p = &x;
	int c = 1;
	if(sizeof(*(c ? (void*)0 : p)) != sizeof(int))
		return 2;
	if(_Generic(c ? p : (void*)0, int*: 0, default: 1))
		return 3;
	if(*(c ? p : 0) != 7)
		return 4;
	return 0;
}
