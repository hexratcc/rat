// e01 dot f32        larsen & amarasinghe pldi 2000 (dot product)
// e02 dot i32        integer variant of e01 (reassociation legal without fast-math)
// e03 rgb2gray f32   larsen & amarasinghe pldi 2000 (color conversion)
// e04 cmul soa f32   porpodas et al cgo 2015 pslp (complex arithmetic, split arrays)
// e05 cmul aos f32   same, interleaved re/im (alternate-opcode add/sub lanes)
// e06 fir4 f32       larsen & amarasinghe pldi 2000 (fir filter)
// e07 mat4mul f32    classic superword example, goslp oopsla 2018 kernel set
// e08 quatmul f32    porpodas et al cgo 2019 super-node slp (sign-mixed chains)
// e09 particle f32   larsen & amarasinghe pldi 2000 (particle simulation, xyzw aos)
// e10 saxpy i32      blas staple, integer lanes (needs packed i32 mul)

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(__GNUC__) || defined(__clang__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

typedef unsigned long long u64;
typedef unsigned int u32;

static double now_ms(void) { return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC; }

#define N 1024
static float fA[N], fB[N], fC[N], fD[N], fE[N], fF[N];
static int iA[N], iB[N], iC[N];

// floats seeded into [1,2): no denormals, no overflow in accumulators
static void seed(void) {
	u32 s = 0x9e3779b9u;
	for(int i = 0; i < N; ++i) {
		s = s * 1664525u + 1013904223u;
		fA[i] = 1.0f + (float)(s >> 9 & 0x7fffff) * (1.0f / 8388608.0f);
		fB[i] = 1.0f + (float)(s >> 5 & 0x7fffff) * (1.0f / 8388608.0f);
		fC[i] = 1.0f + (float)(s >> 2 & 0x7fffff) * (1.0f / 8388608.0f);
		fD[i] = 0.0f;
		fE[i] = 1.0f;
		fF[i] = 0.0f;
		iA[i] = (int)(s >> 20) - 128;
		iB[i] = (int)(s >> 12 & 0xff) - 128;
		iC[i] = 0;
	}
}

static NOINLINE float e01_dot_f32(const float* a, const float* b) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] + a[4] * b[4] + a[5] * b[5]
	     + a[6] * b[6] + a[7] * b[7];
}

static NOINLINE int e02_dot_i32(const int* a, const int* b) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] + a[4] * b[4] + a[5] * b[5]
	     + a[6] * b[6] + a[7] * b[7];
}

static NOINLINE void e03_rgb2gray_f32(float* y, const float* r, const float* g, const float* b) {
	y[0] = 0.299f * r[0] + 0.587f * g[0] + 0.114f * b[0];
	y[1] = 0.299f * r[1] + 0.587f * g[1] + 0.114f * b[1];
	y[2] = 0.299f * r[2] + 0.587f * g[2] + 0.114f * b[2];
	y[3] = 0.299f * r[3] + 0.587f * g[3] + 0.114f * b[3];
}

static NOINLINE void e04_cmul_soa_f32(float* cr,
                                      float* ci,
                                      const float* ar,
                                      const float* ai,
                                      const float* br,
                                      const float* bi) {
	cr[0] = ar[0] * br[0] - ai[0] * bi[0];
	cr[1] = ar[1] * br[1] - ai[1] * bi[1];
	cr[2] = ar[2] * br[2] - ai[2] * bi[2];
	cr[3] = ar[3] * br[3] - ai[3] * bi[3];
	ci[0] = ar[0] * bi[0] + ai[0] * br[0];
	ci[1] = ar[1] * bi[1] + ai[1] * br[1];
	ci[2] = ar[2] * bi[2] + ai[2] * br[2];
	ci[3] = ar[3] * bi[3] + ai[3] * br[3];
}

static NOINLINE void e05_cmul_aos_f32(float* c, const float* a, const float* b) {
	c[0] = a[0] * b[0] - a[1] * b[1];
	c[1] = a[0] * b[1] + a[1] * b[0];
	c[2] = a[2] * b[2] - a[3] * b[3];
	c[3] = a[2] * b[3] + a[3] * b[2];
}

static NOINLINE void e06_fir4_f32(float* y, const float* x, const float* h) {
	y[0] = x[0] * h[0] + x[1] * h[1] + x[2] * h[2] + x[3] * h[3];
	y[1] = x[1] * h[0] + x[2] * h[1] + x[3] * h[2] + x[4] * h[3];
	y[2] = x[2] * h[0] + x[3] * h[1] + x[4] * h[2] + x[5] * h[3];
	y[3] = x[3] * h[0] + x[4] * h[1] + x[5] * h[2] + x[6] * h[3];
}

static NOINLINE void e07_mat4mul_f32(float* C, const float* A, const float* B) {
#define ROW(i) \
	C[4 * i + 0] = A[4 * i + 0] * B[0] + A[4 * i + 1] * B[4] + A[4 * i + 2] * B[8] \
	             + A[4 * i + 3] * B[12]; \
	C[4 * i + 1] = A[4 * i + 0] * B[1] + A[4 * i + 1] * B[5] + A[4 * i + 2] * B[9] \
	             + A[4 * i + 3] * B[13]; \
	C[4 * i + 2] = A[4 * i + 0] * B[2] + A[4 * i + 1] * B[6] + A[4 * i + 2] * B[10] \
	             + A[4 * i + 3] * B[14]; \
	C[4 * i + 3] = A[4 * i + 0] * B[3] + A[4 * i + 1] * B[7] + A[4 * i + 2] * B[11] \
	             + A[4 * i + 3] * B[15];
	ROW(0) ROW(1) ROW(2) ROW(3)
#undef ROW
}

static NOINLINE void e08_quatmul_f32(float* r, const float* p, const float* q) {
	r[0] = p[0] * q[0] - p[1] * q[1] - p[2] * q[2] - p[3] * q[3];
	r[1] = p[0] * q[1] + p[1] * q[0] + p[2] * q[3] - p[3] * q[2];
	r[2] = p[0] * q[2] - p[1] * q[3] + p[2] * q[0] + p[3] * q[1];
	r[3] = p[0] * q[3] + p[1] * q[2] - p[2] * q[1] + p[3] * q[0];
}

static NOINLINE void e09_particle_f32(float* p, const float* v, float dt) {
	p[0] = p[0] + v[0] * dt;
	p[1] = p[1] + v[1] * dt;
	p[2] = p[2] + v[2] * dt;
	p[3] = p[3] + v[3] * dt;
	p[4] = p[4] + v[4] * dt;
	p[5] = p[5] + v[5] * dt;
	p[6] = p[6] + v[6] * dt;
	p[7] = p[7] + v[7] * dt;
}

static NOINLINE void e10_saxpy_i32(int* c, const int* a, const int* b, int s) {
	c[0] = a[0] * s + b[0];
	c[1] = a[1] * s + b[1];
	c[2] = a[2] * s + b[2];
	c[3] = a[3] * s + b[3];
	c[4] = a[4] * s + b[4];
	c[5] = a[5] * s + b[5];
	c[6] = a[6] * s + b[6];
	c[7] = a[7] * s + b[7];
}

static float (*volatile fp_e01)(const float*, const float*) = e01_dot_f32;
static int (*volatile fp_e02)(const int*, const int*) = e02_dot_i32;
static void (*volatile fp_e03)(float*, const float*, const float*, const float*) = e03_rgb2gray_f32;
static void (*volatile fp_e04)(float*, float*, const float*, const float*, const float*, const float*) =
		e04_cmul_soa_f32;
static void (*volatile fp_e05)(float*, const float*, const float*) = e05_cmul_aos_f32;
static void (*volatile fp_e06)(float*, const float*, const float*) = e06_fir4_f32;
static void (*volatile fp_e07)(float*, const float*, const float*) = e07_mat4mul_f32;
static void (*volatile fp_e08)(float*, const float*, const float*) = e08_quatmul_f32;
static void (*volatile fp_e09)(float*, const float*, float) = e09_particle_f32;
static void (*volatile fp_e10)(int*, const int*, const int*, int) = e10_saxpy_i32;

static float gsF = 0.0f;
static int gsI = 0;

static u64 mix(u64 h, u64 v) {
	h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
	return h;
}

static u64 checksum(void) {
	u64 h = 0;
	for(int i = 0; i < N; ++i) {
		union { float f; u32 u; } cf;
		cf.f = fD[i];
		h = mix(h, cf.u);
		cf.f = fE[i];
		h = mix(h, cf.u);
		cf.f = fF[i];
		h = mix(h, cf.u);
		h = mix(h, (u64)(u32)iC[i]);
	}
	union { float f; u32 u; } cs;
	cs.f = gsF;
	h = mix(h, cs.u);
	h = mix(h, (u64)(u32)gsI);
	return h;
}

#define TIME(name, body) \
	do { \
		double t0 = now_ms(); \
		for(int r = 0; r < reps; ++r) { \
			body \
		} \
		double t1 = now_ms(); \
		printf("%-16s %9.3f\n", name, t1 - t0); \
	} while(0)

int main(int argc, char** argv) {
	int reps = argc > 1 ? atoi(argv[1]) : 20000;
	seed();

	TIME("e01_dot_f32", {
		for(int base = 0; base + 8 <= N; base += 8)
			gsF += fp_e01(fA + base, fB + base);
	});
	TIME("e02_dot_i32", {
		for(int base = 0; base + 8 <= N; base += 8)
			gsI += fp_e02(iA + base, iB + base);
	});
	TIME("e03_rgb2gray_f32", {
		for(int base = 0; base + 4 <= N; base += 4)
			fp_e03(fD + base, fA + base, fB + base, fC + base);
	});
	TIME("e04_cmul_soa_f32", {
		for(int base = 0; base + 4 <= N; base += 4)
			fp_e04(fE + base, fF + base, fA + base, fB + base, fC + base, fD + base);
	});
	TIME("e05_cmul_aos_f32", {
		for(int base = 0; base + 4 <= N; base += 4)
			fp_e05(fF + base, fA + base, fB + base);
	});
	TIME("e06_fir4_f32", {
		for(int base = 0; base + 8 <= N; base += 8)
			fp_e06(fD + base, fA + base, fB);
	});
	TIME("e07_mat4mul_f32", {
		for(int base = 0; base + 16 <= N; base += 16)
			fp_e07(fE + base, fA + base, fB + base);
	});
	TIME("e08_quatmul_f32", {
		for(int base = 0; base + 4 <= N; base += 4)
			fp_e08(fF + base, fA + base, fB + base);
	});
	TIME("e09_particle_f32", {
		for(int base = 0; base + 8 <= N; base += 8)
			fp_e09(fE + base, fC + base, 0.0625f);
	});
	TIME("e10_saxpy_i32", {
		for(int base = 0; base + 8 <= N; base += 8)
			fp_e10(iC + base, iA + base, iB + base, 3);
	});

	printf("check %llx\n", (unsigned long long)checksum());
	return 0;
}
