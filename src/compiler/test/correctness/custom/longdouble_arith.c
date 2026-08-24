// expect: 42
int main(void) {
	long double a = 7.0L, b = 3.0L;

	long double sum = a + b;	// 10
	long double dif = a - b;	// 4
	long double pro = a * b;	// 21
	long double quo = a / b;	// 2.333..
	long double neg = -b;			// -3

	int acc = 0;
	acc += (int)sum;					 // 10
	acc += (int)dif;					 // 4   -> 14
	acc += (int)pro;					 // 21  -> 35
	acc += (int)quo;					 // 2   -> 37
	acc += (int)neg + 3;			 // 0   -> 37 (neg == -3)

	if (a > b) acc += 1;			 // 38
	if (b < a) acc += 1;			 // 39
	if (a >= a) acc += 1;			 // 40
	if (b <= b) acc += 1;			 // 41
	if (a != b) acc += 1;			 // 42
	if (a == b) acc += 100;		 // not taken

	return acc;
}
