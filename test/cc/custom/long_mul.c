// expect: 100000
// 64-bit multiply avoids the 32-bit overflow: 100000*100000/100000.
int main(void) {
	long a = 100000;
	long b = 100000;
	return (int)(a * b / 100000);
}
