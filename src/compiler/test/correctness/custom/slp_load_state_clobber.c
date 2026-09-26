// expect: 1

double G;

double f(double a, int i) {
	double d[32];
	for(int k = 0; k < 32; k++)
		d[k] = 0.75;
	double* q = d + i;
	long s[1] = {0};
	if(G)
		;
	double* p = d + (a < 1e15 ? 6 : 3);
	double v = p[12];
	q[9] += 7;
	return v;
}

int main(void) {
	return f(0, 9) == 0.75;
}
