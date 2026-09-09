// expect: 0
// a cast to _Bool yields 0 or 1, never the low bit of the value (C11 6.3.1.2).
// the constant fold used to mask to bit 0, so (_Bool)2 became 0.
static const int s2 = (_Bool)2;
static const int s3 = (_Bool)3;
static const int s256 = (_Bool)256;
enum { E2 = (_Bool)2 };

int main(void) {
	_Bool r = (_Bool)2;
	if(s2 != 1 || s3 != 1 || s256 != 1)
		return 1;
	if(E2 != 1 || (int)r != 1)
		return 2;
	return 0;
}
