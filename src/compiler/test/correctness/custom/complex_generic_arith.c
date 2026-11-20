// expect: 0

int main(void) {
	double _Complex z = 1;
	float _Complex f = 2;
	if(_Generic(z * 2.0, double _Complex: 0, default: 1))
		return 1;
	if(_Generic(f + f, float _Complex: 0, default: 1))
		return 2;
	return 0;
}
