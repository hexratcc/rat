// expect: 0
// packed loops with an int splat of a loaded scalar (loaded straight into an xmm) and x op x on
// floats (op reads the copy of x)

int X[64], Y[64];
float F[64], G[64];

__attribute__((noinline)) void saxpy(int* pa) { // *pa may alias Y: one load per iteration
	for(int i = 0; i < 64; i += 4) {
		int a = *pa;
		Y[i] = a * X[i];
		Y[i + 1] = a * X[i + 1];
		Y[i + 2] = a * X[i + 2];
		Y[i + 3] = a * X[i + 3];
	}
}

__attribute__((noinline)) void self(void) {
	for(int i = 0; i < 64; i += 4) {
		G[i] = F[i] * F[i];
		G[i + 1] = F[i + 1] * F[i + 1];
		G[i + 2] = F[i + 2] * F[i + 2];
		G[i + 3] = F[i + 3] * F[i + 3];
	}
}

__attribute__((noinline)) float diff(float x) { return x - x + x / x; }

int main(void) {
	int a = -3;
	for(int i = 0; i < 64; i++) {
		X[i] = i - 20;
		F[i] = (float)i - 0.5f;
	}
	saxpy(&a);
	for(int i = 0; i < 64; i++)
		if(Y[i] != -3 * (i - 20))
			return 1;
	Y[4] = 2;
	saxpy(&Y[4]); // the splat value changes after Y[4] is written
	if(Y[3] != 2 * X[3] || Y[4] != 2 * X[4] || Y[7] != 2 * X[7] || Y[8] != 2 * X[4] * X[8])
		return 2;
	self();
	for(int i = 0; i < 64; i++)
		if(G[i] != F[i] * F[i])
			return 3;
	if(diff(2.5f) != 1.0f)
		return 4;
	return 0;
}
