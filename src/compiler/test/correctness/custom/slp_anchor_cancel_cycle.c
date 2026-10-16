// expect: 0
// one block: the run into q reads lanes through r = &p[i + 1] - i, whose index terms cancel. The
// reduction over p[0..7] reads the entry state, so its wide loads run before q[8] = 1, and their
// pointer must not be r's, which needs i, a load after q[8] (a cycle in the block's schedule)
int P[64], Q[64];

__attribute__((noinline)) int f(int *p, int *q, int *idx) {
	int s = p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
	q[8] = 1;
	int i = *idx;
	int *r = &p[i + 1] - i;
	int a = r[-1], b = r[0], c = r[1], d = r[2];
	q[1] = a * 3 + 1;
	q[2] = b * 3 + 1;
	q[3] = c * 3 + 1;
	q[4] = d * 3 + 1;
	return s;
}

int main(void) {
	int idx = 5;
	for(int k = 0; k < 64; k++)
		P[k] = k * 7 + 1;
	if(f(P, Q, &idx) != 204 || Q[1] != 4 || Q[2] != 25 || Q[3] != 46 || Q[4] != 67)
		return 1;
	if(f(P, P, &idx) != 204 || P[1] != 4 || P[2] != 25 || P[3] != 46 || P[4] != 67 || P[8] != 1)
		return 2;
	return 0;
}
