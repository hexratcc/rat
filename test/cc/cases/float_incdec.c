// expect: 5
// Increment / decrement and compound assignment on floating values.
int main(void) {
	double d = 1.0;
	++d;        // 2.0
	d += 2.5;   // 4.5
	float f = 3.0f;
	--f;        // 2.0
	f *= 0.5f;  // 1.0
	return (int)d + (int)f; // 4 + 1 = 5
}
