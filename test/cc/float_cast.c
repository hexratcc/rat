// expect: 7
// Conversions: int -> double -> float and back; 3 + 4 == 7.
int main(void) {
	int i = 3;
	double d = i + 0.9;   // 3.9
	float f = (float)d;   // 3.9f
	int back = (int)f;    // truncates to 3
	double e = 4.5;
	int n = (int)e;       // 4
	return back + n;
}
