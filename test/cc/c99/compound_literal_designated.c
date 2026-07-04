// expect: 0
#include "test.h"

// C99 6.7.8p22: an unsized array's bound is inferred from its initializer,
// honoring designators. A file-scope compound literal of unsized array type
// must deduce its size the same way a declared unsized array does, so the
// largest designated index (here 5) sets the element count to 6, even though
// only one initializer is present.

int *p = (int[]){ [5] = 42 };

// designators interleaved with positional elements: index 0 is set, then a
// jump to index 4 sets the high water mark, so the array has 5 elements.
int *q = (int[]){ 7, [4] = 9 };

int main() {
	// file-scope declared array, same designator, infers size 6 for comparison
	int a[] = { [5] = 42 };
	ASSERT(6, (int)(sizeof a / sizeof a[0]));

	ASSERT(42, p[5]);
	ASSERT(0, p[0]);

	ASSERT(7, q[0]);
	ASSERT(9, q[4]);
	ASSERT(0, q[1]);

	printf("OK\n");
	return 0;
}
