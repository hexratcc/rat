// expect: 0
// output:
//| signbitf 0 1 0 1 0 1
//| signbit 0 1 0 1 0 1
//| signbitl 0 1 0 1 0 1
//| isinff 0 0 1 1 0
//| isinf 0 0 1 1 0
//| isinfl 0 0 1 1 0
//| isnanf 0 0 0 0 1
//| isnan 0 0 0 0 1
//| isnanl 0 0 0 0 1
//| isfinite 1 1 0 0 0
#include <stdio.h>

volatile float fz = 0.0f, fone = 1.0f;
volatile double dz = 0.0, done = 1.0;
volatile long double lz = 0.0L, lone = 1.0L;

int main(void) {
	float fn = -fz, fi = fone / fz, fnan = fz / fz;
	double dn = -dz, di = done / dz, dnan = dz / dz;
	long double ln = -lz, li = lone / lz, lnan = lz / lz;

	printf("signbitf %d %d %d %d %d %d\n",
				 !!__builtin_signbitf(fz),
				 !!__builtin_signbitf(fn),
				 !!__builtin_signbitf(fone),
				 !!__builtin_signbitf(-fone),
				 !!__builtin_signbitf(fi),
				 !!__builtin_signbitf(-fi));
	printf("signbit %d %d %d %d %d %d\n",
				 !!__builtin_signbit(dz),
				 !!__builtin_signbit(dn),
				 !!__builtin_signbit(done),
				 !!__builtin_signbit(-done),
				 !!__builtin_signbit(di),
				 !!__builtin_signbit(-di));
	printf("signbitl %d %d %d %d %d %d\n",
				 !!__builtin_signbitl(lz),
				 !!__builtin_signbitl(ln),
				 !!__builtin_signbitl(lone),
				 !!__builtin_signbitl(-lone),
				 !!__builtin_signbitl(li),
				 !!__builtin_signbitl(-li));

	printf("isinff %d %d %d %d %d\n",
				 !!__builtin_isinff(fz),
				 !!__builtin_isinff(fone),
				 !!__builtin_isinff(fi),
				 !!__builtin_isinff(-fi),
				 !!__builtin_isinff(fnan));
	printf("isinf %d %d %d %d %d\n",
				 !!__builtin_isinf(dz),
				 !!__builtin_isinf(done),
				 !!__builtin_isinf(di),
				 !!__builtin_isinf(-di),
				 !!__builtin_isinf(dnan));
	printf("isinfl %d %d %d %d %d\n",
				 !!__builtin_isinfl(lz),
				 !!__builtin_isinfl(lone),
				 !!__builtin_isinfl(li),
				 !!__builtin_isinfl(-li),
				 !!__builtin_isinfl(lnan));

	printf("isnanf %d %d %d %d %d\n",
				 !!__builtin_isnanf(fz),
				 !!__builtin_isnanf(fone),
				 !!__builtin_isnanf(fi),
				 !!__builtin_isnanf(-fi),
				 !!__builtin_isnanf(fnan));
	printf("isnan %d %d %d %d %d\n",
				 !!__builtin_isnan(dz),
				 !!__builtin_isnan(done),
				 !!__builtin_isnan(di),
				 !!__builtin_isnan(-di),
				 !!__builtin_isnan(dnan));
	printf("isnanl %d %d %d %d %d\n",
				 !!__builtin_isnanl(lz),
				 !!__builtin_isnanl(lone),
				 !!__builtin_isnanl(li),
				 !!__builtin_isnanl(-li),
				 !!__builtin_isnanl(lnan));

	printf("isfinite %d %d %d %d %d\n",
				 !!__builtin_isfinite(dz),
				 !!__builtin_isfinite(done),
				 !!__builtin_isfinite(di),
				 !!__builtin_isfinite(-di),
				 !!__builtin_isfinite(dnan));
	return 0;
}
