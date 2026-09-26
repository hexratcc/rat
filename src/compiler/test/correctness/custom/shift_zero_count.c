// expect: 0
// passes:

#include <stdint.h>

volatile uint32_t u32 = 0x84222325u;
volatile int32_t i32 = -0x7bddcadb;
volatile uint16_t u16 = 0x8423;
volatile uint8_t u8 = 0x85;
volatile uint64_t u64 = 0x84222325deadbeefull;
volatile int zero = 0;
volatile int one = 1;

int main(void) {
	int bad = 0;
	int32_t r = u32 >> zero;
	bad |= (int64_t)r != -0x7bdddcdbll;
	r = u32 >> 0;
	bad |= ((int64_t)r != -0x7bdddcdbll) << 1;
	r = u32 >> one;
	bad |= ((int64_t)r != 0x42111192ll) << 2;
	r = u32 << zero;
	bad |= ((int64_t)r != -0x7bdddcdbll) << 3;
	r = i32 >> zero;
	bad |= ((int64_t)r != -0x7bddcadbll) << 4;
	int16_t h = u16 >> zero;
	bad |= ((int64_t)h != -0x7bdd) << 5;
	int8_t b = u8 >> zero;
	bad |= ((int64_t)b != -0x7b) << 6;
	int64_t w = u64 >> zero;
	bad |= (w != (int64_t)0x84222325deadbeefull) << 7;
	return bad;
}
