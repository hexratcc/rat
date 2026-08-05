#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef unsigned long long u64;
typedef unsigned int u32;

static double now_ms(void) { return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC; }

#define N 4096
static int ia[N], ib[N], ic[N];
static float fa[N], fb[N], fc[N];
static double da[N], db[N], dc[N];

static void seed(void) {
	u32 s = 0x9e3779b9u;
	for(int i = 0; i < N; ++i) {
		s = s * 1664525u + 1013904223u;
		ia[i] = (int)(s >> 8) - 4096;
		ib[i] = (int)(s >> 16) - 128;
		fa[i] = (float)(int)(s >> 20) * 0.25f;
		fb[i] = (float)(int)(s >> 12) * 0.0625f;
		da[i] = (double)(int)(s >> 10) * 0.125;
		db[i] = (double)(int)(s >> 14) * 0.5;
	}
}

// 4-lane int add/mask block, unrolled by 16
static void k_int_block(int base) {
#define I1(k) ic[base + k] = (ia[base + k] + ib[base + k]) ^ (ia[base + k] & ib[base + k]);
#define I4(k) I1(k) I1(k + 1) I1(k + 2) I1(k + 3)
	I4(0) I4(4) I4(8) I4(12)
#undef I4
#undef I1
}

// 4-lane f32 fma-shaped block, unrolled by 16
static void k_f32_block(int base) {
#define F1(k) fc[base + k] = fa[base + k] * fb[base + k] + fa[base + k];
#define F4(k) F1(k) F1(k + 1) F1(k + 2) F1(k + 3)
	F4(0) F4(4) F4(8) F4(12)
#undef F4
#undef F1
}

// 2-lane f64 block, unrolled by 8
static void k_f64_block(int base) {
#define D1(k) dc[base + k] = (da[base + k] + db[base + k]) * db[base + k];
#define D2(k) D1(k) D1(k + 1)
	D2(0) D2(2) D2(4) D2(6)
#undef D2
#undef D1
}

// 4x4 f32 matrix add at a base offset, the classic superword example
static void k_mat4_add(int c0, int a0, int b0) {
	for(int r = 0; r < 16; r += 4) {
		fc[c0 + r + 0] = fa[a0 + r + 0] + fb[b0 + r + 0];
		fc[c0 + r + 1] = fa[a0 + r + 1] + fb[b0 + r + 1];
		fc[c0 + r + 2] = fa[a0 + r + 2] + fb[b0 + r + 2];
		fc[c0 + r + 3] = fa[a0 + r + 3] + fb[b0 + r + 3];
	}
}

// pointer-parameter kernels
static void kp_int(int* c, const int* a, const int* b) {
#define P1(k) c[k] = (a[k] + b[k]) ^ (a[k] & b[k]);
	P1(0) P1(1) P1(2) P1(3) P1(4) P1(5) P1(6) P1(7)
	P1(8) P1(9) P1(10) P1(11) P1(12) P1(13) P1(14) P1(15)
#undef P1
}

static void kp_f32(float* c, const float* a, const float* b) {
#define P1(k) c[k] = a[k] * b[k] + a[k];
	P1(0) P1(1) P1(2) P1(3) P1(4) P1(5) P1(6) P1(7)
	P1(8) P1(9) P1(10) P1(11) P1(12) P1(13) P1(14) P1(15)
#undef P1
}

static void kp_f64(double* c, const double* a, const double* b) {
#define P1(k) c[k] = (a[k] + b[k]) * b[k];
	P1(0) P1(1) P1(2) P1(3) P1(4) P1(5) P1(6) P1(7)
#undef P1
}

static void (*volatile fp_int)(int*, const int*, const int*) = kp_int;
static void (*volatile fp_f32)(float*, const float*, const float*) = kp_f32;
static void (*volatile fp_f64)(double*, const double*, const double*) = kp_f64;

static u64 mix(u64 h, u64 v) {
	h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
	return h;
}

static u64 checksum(void) {
	u64 h = 0;
	for(int i = 0; i < N; ++i) {
		h = mix(h, (u64)(u32)ic[i]);
		union { float f; u32 u; } cf;
		cf.f = fc[i];
		h = mix(h, cf.u);
		union { double d; u64 u; } cd;
		cd.d = dc[i];
		h = mix(h, cd.u);
	}
	return h;
}

int main(int argc, char** argv) {
	int reps = argc > 1 ? atoi(argv[1]) : 2000;
	seed();
	double t0 = now_ms();
	for(int r = 0; r < reps; ++r) {
		for(int base = 0; base < N; base += 16)
			k_int_block(base);
		for(int base = 0; base < N; base += 16)
			k_f32_block(base);
		for(int base = 0; base < N; base += 8)
			k_f64_block(base);
		for(int base = 0; base + 48 <= N; base += 48)
			k_mat4_add(base, base + 16, base + 32);
	}
	double t1 = now_ms();
	for(int r = 0; r < reps; ++r) {
		for(int base = 0; base + 16 <= N; base += 16)
			fp_int(ic + base, ia + base, ib + base);
		for(int base = 0; base + 16 <= N; base += 16)
			fp_f32(fc + base, fa + base, fb + base);
		for(int base = 0; base + 8 <= N; base += 8)
			fp_f64(dc + base, da + base, db + base);
	}
	double t2 = now_ms();
	printf("global   %8.2f ms\n", t1 - t0);
	printf("pointer  %8.2f ms\n", t2 - t1);
	printf("total    %8.2f ms\n", t2 - t0);
	printf("check %llx\n", (unsigned long long)checksum());
	return 0;
}
