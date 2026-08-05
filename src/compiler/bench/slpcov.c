#if defined(__GNUC__) || defined(__clang__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

typedef long long i64;
typedef unsigned int u32;

int gi_a[32], gi_b[32], gi_c[32];
i64 gl_a[16], gl_b[16], gl_c[16];
float gf_a[32], gf_b[32], gf_c[32];
double gd_a[16], gd_b[16], gd_c[16];

// global-array kernels

NOINLINE void g01_i32_add(void) {
	gi_c[0] = gi_a[0] + gi_b[0];
	gi_c[1] = gi_a[1] + gi_b[1];
	gi_c[2] = gi_a[2] + gi_b[2];
	gi_c[3] = gi_a[3] + gi_b[3];
}

NOINLINE void g02_i32_chain(void) {
	gi_c[0] = (gi_a[0] + gi_b[0]) ^ (gi_a[0] & gi_b[0]);
	gi_c[1] = (gi_a[1] + gi_b[1]) ^ (gi_a[1] & gi_b[1]);
	gi_c[2] = (gi_a[2] + gi_b[2]) ^ (gi_a[2] & gi_b[2]);
	gi_c[3] = (gi_a[3] + gi_b[3]) ^ (gi_a[3] & gi_b[3]);
}
NOINLINE void g03_i32_splat(int k) {
	gi_c[0] = gi_a[0] + k;
	gi_c[1] = gi_a[1] + k;
	gi_c[2] = gi_a[2] + k;
	gi_c[3] = gi_a[3] + k;
}
NOINLINE void g04_i32_fill(int k) {
	gi_c[0] = k;
	gi_c[1] = k;
	gi_c[2] = k;
	gi_c[3] = k;
}
NOINLINE void g05_i32_constfill(void) {
	gi_c[0] = 11;
	gi_c[1] = 22;
	gi_c[2] = 33;
	gi_c[3] = 44;
}
NOINLINE void g06_i32_copy(void) {
	gi_c[0] = gi_a[0];
	gi_c[1] = gi_a[1];
	gi_c[2] = gi_a[2];
	gi_c[3] = gi_a[3];
}
NOINLINE void g07_i32_swapped(void) {
	gi_c[0] = gi_a[0] + gi_b[0];
	gi_c[1] = gi_b[1] + gi_a[1];
	gi_c[2] = gi_a[2] + gi_b[2];
	gi_c[3] = gi_b[3] + gi_a[3];
}
NOINLINE void g08_i32_inplace(void) {
	gi_c[0] = gi_c[0] + gi_a[0];
	gi_c[1] = gi_c[1] + gi_a[1];
	gi_c[2] = gi_c[2] + gi_a[2];
	gi_c[3] = gi_c[3] + gi_a[3];
}
NOINLINE void g09_i32_wide8(void) {
	gi_c[0] = gi_a[0] ^ gi_b[0];
	gi_c[1] = gi_a[1] ^ gi_b[1];
	gi_c[2] = gi_a[2] ^ gi_b[2];
	gi_c[3] = gi_a[3] ^ gi_b[3];
	gi_c[4] = gi_a[4] ^ gi_b[4];
	gi_c[5] = gi_a[5] ^ gi_b[5];
	gi_c[6] = gi_a[6] ^ gi_b[6];
	gi_c[7] = gi_a[7] ^ gi_b[7];
}
NOINLINE void g10_i32_reversed_stores(void) {
	gi_c[3] = gi_a[3] | gi_b[3];
	gi_c[2] = gi_a[2] | gi_b[2];
	gi_c[1] = gi_a[1] | gi_b[1];
	gi_c[0] = gi_a[0] | gi_b[0];
}
NOINLINE void g11_i64_add(void) {
	gl_c[0] = gl_a[0] + gl_b[0];
	gl_c[1] = gl_a[1] + gl_b[1];
}
NOINLINE void g12_i64_sub_and(void) {
	gl_c[0] = (gl_a[0] - gl_b[0]) & gl_a[0];
	gl_c[1] = (gl_a[1] - gl_b[1]) & gl_a[1];
}
NOINLINE void g13_f32_mul(void) {
	gf_c[0] = gf_a[0] * gf_b[0];
	gf_c[1] = gf_a[1] * gf_b[1];
	gf_c[2] = gf_a[2] * gf_b[2];
	gf_c[3] = gf_a[3] * gf_b[3];
}
NOINLINE void g14_f32_fma_shape(void) {
	gf_c[0] = gf_a[0] * gf_b[0] + gf_a[0];
	gf_c[1] = gf_a[1] * gf_b[1] + gf_a[1];
	gf_c[2] = gf_a[2] * gf_b[2] + gf_a[2];
	gf_c[3] = gf_a[3] * gf_b[3] + gf_a[3];
}
NOINLINE void g15_f32_div(void) {
	gf_c[0] = gf_a[0] / gf_b[0];
	gf_c[1] = gf_a[1] / gf_b[1];
	gf_c[2] = gf_a[2] / gf_b[2];
	gf_c[3] = gf_a[3] / gf_b[3];
}
NOINLINE void g16_f32_splat(float k) {
	gf_c[0] = gf_a[0] * k;
	gf_c[1] = gf_a[1] * k;
	gf_c[2] = gf_a[2] * k;
	gf_c[3] = gf_a[3] * k;
}
NOINLINE void g17_f64_add(void) {
	gd_c[0] = gd_a[0] + gd_b[0];
	gd_c[1] = gd_a[1] + gd_b[1];
}
NOINLINE void g18_f64_chain(void) {
	gd_c[0] = (gd_a[0] + gd_b[0]) * gd_b[0];
	gd_c[1] = (gd_a[1] + gd_b[1]) * gd_b[1];
}
NOINLINE void g19_mixed_offsets(int base) {
	gi_c[base + 0] = gi_a[base + 0] + gi_b[base + 0];
	gi_c[base + 1] = gi_a[base + 1] + gi_b[base + 1];
	gi_c[base + 2] = gi_a[base + 2] + gi_b[base + 2];
	gi_c[base + 3] = gi_a[base + 3] + gi_b[base + 3];
}
NOINLINE void g20_i32_shared_subexpr(void) {
	int t = gi_a[0] + gi_b[0];
	gi_c[0] = t + gi_a[0];
	gi_c[1] = t + gi_a[1];
	gi_c[2] = t + gi_a[2];
	gi_c[3] = t + gi_a[3];
}

// pointer-parameter kernels

NOINLINE void p01_i32_add(int* c, const int* a, const int* b) {
	c[0] = a[0] + b[0];
	c[1] = a[1] + b[1];
	c[2] = a[2] + b[2];
	c[3] = a[3] + b[3];
}
NOINLINE void p02_i32_chain(int* c, const int* a, const int* b) {
	c[0] = (a[0] + b[0]) ^ (a[0] & b[0]);
	c[1] = (a[1] + b[1]) ^ (a[1] & b[1]);
	c[2] = (a[2] + b[2]) ^ (a[2] & b[2]);
	c[3] = (a[3] + b[3]) ^ (a[3] & b[3]);
}
NOINLINE void p03_i32_splat(int* c, const int* a, int k) {
	c[0] = a[0] + k;
	c[1] = a[1] + k;
	c[2] = a[2] + k;
	c[3] = a[3] + k;
}
NOINLINE void p04_i32_wide16(int* c, const int* a, const int* b) {
	c[0] = a[0] + b[0];
	c[1] = a[1] + b[1];
	c[2] = a[2] + b[2];
	c[3] = a[3] + b[3];
	c[4] = a[4] + b[4];
	c[5] = a[5] + b[5];
	c[6] = a[6] + b[6];
	c[7] = a[7] + b[7];
	c[8] = a[8] + b[8];
	c[9] = a[9] + b[9];
	c[10] = a[10] + b[10];
	c[11] = a[11] + b[11];
	c[12] = a[12] + b[12];
	c[13] = a[13] + b[13];
	c[14] = a[14] + b[14];
	c[15] = a[15] + b[15];
}
NOINLINE void p05_i32_offset(int* c, const int* a, const int* b, int i) {
	c[i + 0] = a[i + 0] + b[i + 0];
	c[i + 1] = a[i + 1] + b[i + 1];
	c[i + 2] = a[i + 2] + b[i + 2];
	c[i + 3] = a[i + 3] + b[i + 3];
}
NOINLINE void p06_f32_fma_shape(float* c, const float* a, const float* b) {
	c[0] = a[0] * b[0] + a[0];
	c[1] = a[1] * b[1] + a[1];
	c[2] = a[2] * b[2] + a[2];
	c[3] = a[3] * b[3] + a[3];
}
NOINLINE void p07_f64_muladd(double* c, const double* a, const double* b) {
	c[0] = (a[0] + b[0]) * b[0];
	c[1] = (a[1] + b[1]) * b[1];
}
NOINLINE void p08_i32_inplace(int* c, const int* a) {
	c[0] = c[0] + a[0];
	c[1] = c[1] + a[1];
	c[2] = c[2] + a[2];
	c[3] = c[3] + a[3];
}
NOINLINE void p09_i32_fill(int* c, int k) {
	c[0] = k;
	c[1] = k;
	c[2] = k;
	c[3] = k;
}
NOINLINE void p10_i32_copy(int* c, const int* a) {
	c[0] = a[0];
	c[1] = a[1];
	c[2] = a[2];
	c[3] = a[3];
}

typedef void (*fn)(void);
volatile fn sink[] = {
		(fn)g01_i32_add,			 (fn)g02_i32_chain,
		(fn)g03_i32_splat,		 (fn)g04_i32_fill,
		(fn)g05_i32_constfill, (fn)g06_i32_copy,
		(fn)g07_i32_swapped,	 (fn)g08_i32_inplace,
		(fn)g09_i32_wide8,		 (fn)g10_i32_reversed_stores,
		(fn)g11_i64_add,			 (fn)g12_i64_sub_and,
		(fn)g13_f32_mul,			 (fn)g14_f32_fma_shape,
		(fn)g15_f32_div,			 (fn)g16_f32_splat,
		(fn)g17_f64_add,			 (fn)g18_f64_chain,
		(fn)g19_mixed_offsets, (fn)g20_i32_shared_subexpr,
		(fn)p01_i32_add,			 (fn)p02_i32_chain,
		(fn)p03_i32_splat,		 (fn)p04_i32_wide16,
		(fn)p05_i32_offset,		 (fn)p06_f32_fma_shape,
		(fn)p07_f64_muladd,		 (fn)p08_i32_inplace,
		(fn)p09_i32_fill,			 (fn)p10_i32_copy,
};

int main(void) { return (int)(sink[0] == 0); }
