// expect: 0

#include <stdalign.h>

_Alignas(64) int gBig;
_Alignas(32) char gArr[40];
alignas(16) static double gStatic[3];
int gPlain;

int main(void) {
	_Alignas(64) int lBig;
	_Alignas(32) char lArr[40];
	alignas(16) double lDouble;
	char pad; // pushes the following slot off a natural boundary
	_Alignas(128) long lHuge;

	if((unsigned long)&gBig & 63u)
		return 1;
	if((unsigned long)gArr & 31u)
		return 2;
	if((unsigned long)gStatic & 15u)
		return 3;
	if((unsigned long)&lBig & 63u)
		return 4;
	if((unsigned long)lArr & 31u)
		return 5;
	if((unsigned long)&lDouble & 15u)
		return 6;
	if((unsigned long)&lHuge & 127u)
		return 7;
	// the objects are still usable and distinct
	lBig = 7;
	lArr[0] = 3;
	lDouble = 1.5;
	lHuge = 9;
	pad = 1;
	gBig = 11;
	gArr[39] = 4;
	gStatic[2] = 2.5;
	gPlain = 5;
	if(lBig + lArr[0] + (int)lDouble + (int)lHuge + pad != 21)
		return 8;
	if(gBig + gArr[39] + (int)gStatic[2] + gPlain != 22)
		return 9;
	return 0;
}
