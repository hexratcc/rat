#ifndef _RATCC_COMPLEX_H
#define _RATCC_COMPLEX_H

#define complex _Complex
#define _Complex_I (0.0f + 1.0fi)
#define I _Complex_I

double creal(double complex z);
double cimag(double complex z);
double cabs(double complex z);
double carg(double complex z);
double complex conj(double complex z);
double complex cexp(double complex z);
double complex clog(double complex z);
double complex csqrt(double complex z);
double complex cpow(double complex a, double complex b);

float crealf(float complex z);
float cimagf(float complex z);
float cabsf(float complex z);
float complex conjf(float complex z);
float complex csqrtf(float complex z);

#endif
