// expect: 0
// a decremented _Bool is 0 or 1 in its register and in memory
volatile _Bool zero = 0;
volatile _Bool one = 1;

int dec(_Bool a) {
	--a;
	return a;
}

int main(void) {
	_Bool x = zero;
	x--;
	if(dec(zero) != 1 || dec(one) != 0)
		return 1;
	if(*(unsigned char*)&x != 1)
		return 2;
	return 0;
}
