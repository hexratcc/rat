// expect: 0
// output:
//| const 8000000000000000
//| runtime 8000000000000000
//| float 80000000
//| inf 1
#include <stdio.h>
#include <string.h>

volatile double dz = 0.0;
volatile float fz = 0.0f;

int main(void) {
	double a = -0.0;
	float f;
	unsigned long long bits;
	unsigned int fbits;
	memcpy(&bits, &a, sizeof bits);
	printf("const %016llx\n", bits);
	a = -dz;
	memcpy(&bits, &a, sizeof bits);
	printf("runtime %016llx\n", bits);
	f = -fz;
	memcpy(&fbits, &f, sizeof fbits);
	printf("float %08x\n", fbits);
	printf("inf %d\n", 1.0 / -dz < 0.0);
	return 0;
}
