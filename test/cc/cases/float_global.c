// expect: 12
// Global double and float array initializers evaluated at compile time.
double scale = 2.5;
float arr[3] = {1.0f, 2.0f, 3.0f};

double square(double x) {
	return x * x;
}

int main(void) {
	double s = square(scale);              // 6.25
	float sum = arr[0] + arr[1] + arr[2];  // 6.0
	return (int)s + (int)sum;
}
