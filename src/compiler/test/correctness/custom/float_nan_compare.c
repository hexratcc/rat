// expect: 0
// output:
//| eq 0 0 0
//| ne 1 1 1
//| br 0 1
#include <stdio.h>

volatile double dz = 0.0;
volatile float fz = 0.0f;
volatile long double lz = 0.0L;

int main(void) {
	double dn = dz / dz;
	float fn = fz / fz;
	long double ln = lz / lz;
	int eqTaken = 0;
	int neTaken = 0;
	printf("eq %d %d %d\n", dn == dn, fn == fn, ln == ln);
	printf("ne %d %d %d\n", dn != dn, fn != fn, ln != ln);
	if(dn == dn)
		eqTaken = 1;
	if(dn != dn)
		neTaken = 1;
	printf("br %d %d\n", eqTaken, neTaken);
	return 0;
}
