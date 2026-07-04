// expect: 15
// A compound literal at file scope has static storage duration and constant
// initializers (C99 6.5.2.5p5). An array compound literal decays to the address
// of its (statically allocated) first element (6.3.2.1p3), so it can initialize
// a file-scope pointer.
int *p = (int[]){ 2, 4, 6, 8 };
int *q = (int[]){ 1, 2, 3 } + 1;	// pointer arithmetic on the decayed address

int main(void) {
	int s = p[0] + p[1] + p[2] + p[3];	// 2+4+6+8 = 20
	int t = q[0] + q[1];			// elements 2 and 3 -> 2+3 = 5
	return s - t;				// 20 - 5 = 15
}
