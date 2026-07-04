// expect: 48
// Compound assignments: 5 -> +=10 (15) -> *=4 (60) -> -=12 (48).
int main(void) {
	int v = 5;
	v += 10;
	v *= 4;
	v -= 12;
	return v;
}
