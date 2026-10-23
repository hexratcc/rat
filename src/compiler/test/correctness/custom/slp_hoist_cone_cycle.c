// expect: 0
// the address decomposition lets the load of O pass the store to G[0], while its address needs a
// load of G after that store, loadcanon must keep the load of O below that store, or the block has
// a cycle, the store to G[3] keeps the load of O off the final state
long O[8] = {0, 1, 2, 3, 4, 5, 6, 7}, G[4];

__attribute__((noinline)) long f(long k) {
	G[0] = 1;
	long j = G[k];
	long r = *(O + 7 - j);
	G[3] = 3;
	return r;
}

int main(void) {
	G[2] = 2;
	return f(2) != 5;
}
