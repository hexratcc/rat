// expect: 0
// passes:

#include <limits.h>

enum neg { n = INT_MIN };
enum pos { p = 1 };

int* ip;
enum neg* np;
enum pos* pp;

int main(void) {
	enum neg x = n;
	enum pos y = p;
	np = &x;
	pp = &y;
	// the composite of int* and a signed enum* stays signed
	if(*(1 ? np : ip) > 0)
		return 1;
	if(*np > 0)
		return 2;
	// an all-non-negative enum stays unsigned
	if((enum pos)-1 < 0)
		return 3;
	if(*pp != 1)
		return 4;
	return 0;
}
