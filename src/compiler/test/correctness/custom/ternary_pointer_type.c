// expect: 0

int pick(int c, char* p, short* s) {
	if(sizeof(c ? p : p) != sizeof(char*))
		return 1;
	if(*(c ? p : p) != 'a')
		return 2;
	if(_Generic(c ? s : s, short*: 0, default: 1))
		return 3;
	return 0;
}

int main(void) {
	char ch = 'a';
	short sh = 0;
	return pick(1, &ch, &sh);
}
