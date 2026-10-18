// expect: 0
// a load hoisted to the block's entry state stays above the guard of a later run however many
// stores lie between, h must read H[0] before the run writes it
unsigned G[8448], H[64];

#define S1(i) G[2 * (i)] = (i);
#define S4(i) S1(i) S1(i + 1) S1(i + 2) S1(i + 3)
#define S16(i) S4(i) S4(i + 4) S4(i + 8) S4(i + 12)
#define S64(i) S16(i) S16(i + 16) S16(i + 32) S16(i + 48)
#define S256(i) S64(i) S64(i + 64) S64(i + 128) S64(i + 192)
#define S1024(i) S256(i) S256(i + 256) S256(i + 512) S256(i + 768)

__attribute__((noinline)) unsigned k(unsigned *c, unsigned *a) {
	// 4224 strided stores that do not pack
	S1024(0) S1024(1024) S1024(2048) S1024(3072) S64(4096) S64(4160)
	unsigned h = H[0];
	c[0] = a[0] + a[0];
	c[1] = a[1] + a[1];
	c[2] = a[2] + a[2];
	c[3] = a[3] + a[3];
	c[4] = a[4] + a[4];
	c[5] = a[5] + a[5];
	c[6] = a[6] + a[6];
	c[7] = a[7] + a[7];
	return h;
}

int main(void) {
	for(int i = 0; i < 64; i++)
		H[i] = i + 100;
	if(k(H, H + 32) != 100) // vector arm
		return 1;
	if(k(H + 40, H) != 264)
		return 2;
	if(H[0] != 264 || H[40] != 528 || G[8446] != 4223)
		return 3;
	return 0;
}
