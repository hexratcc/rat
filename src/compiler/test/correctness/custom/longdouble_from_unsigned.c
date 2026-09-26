// expect: 0
// output:
//| 0xf.9p+4 0xf.ff9p+12 0xf.ffffff9p+28
//| 0x8p+60 0xf.fffffffffffffffp+60 0x8.0000000fffffffdp+60
//| -0xcp-2
#include <stdint.h>
#include <stdio.h>

uint8_t C = 0xf9;
uint16_t S = 0xfff9;
uint32_t I = 0xfffffff9u;
uint64_t L = 0x8000000000000000ull;
uint64_t M = 0xffffffffffffffffull;
uint64_t K = 0x80000000fffffffdull;
int64_t N = -3;

int main(void) {
	printf("%La %La %La\n", (long double)C, (long double)S, (long double)I);
	printf("%La %La %La\n", (long double)L, (long double)M, (long double)K);
	printf("%La\n", (long double)N);
	return 0;
}
