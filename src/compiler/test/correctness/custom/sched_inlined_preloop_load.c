// expect: 0
// memoryopt lets the load of B[5] after the loop read the state before the loop, inlining gives that
// load a higher node number than the store to B[i] after the call, and no writer of their block reads
// the state before the loop, so the scheduler must still order the load before the store
unsigned B[64], G[64];

static unsigned f(void) {
	unsigned L[64];
	for(int j = 0; j < 64; j++)
		L[j] = G[j];
	return B[5] + L[3];
}

__attribute__((noinline)) unsigned k(int i) {
	unsigned t = f();
	B[i] = 0;
	return t;
}

int main(void) {
	B[5] = 7;
	G[3] = 1;
	return k(5) != 8;
}
