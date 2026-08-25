// expect: 0

int arr[64];
int* gp = arr;
int calls = 0;

int* bump(void) {
	++calls;
	return arr;
}

int main(void) {
	int* q;
	int i;

	// every documented rw / locality combination, plus the defaults
	__builtin_prefetch(arr, 0, 0);
	__builtin_prefetch(arr, 0, 1);
	__builtin_prefetch(arr, 0, 2);
	__builtin_prefetch(arr, 0, 3);
	__builtin_prefetch(arr, 1, 0);
	__builtin_prefetch(arr, 1, 1);
	__builtin_prefetch(arr, 1, 2);
	__builtin_prefetch(arr, 1, 3);
	__builtin_prefetch(arr, 1);
	__builtin_prefetch(arr);
	__builtin_prefetch(&arr[7], 1 - 1, 6 - (2 * 3));

	// the address expression runs, assignments in it included
	__builtin_prefetch((q = gp), 0, 0);
	if(q != arr)
		return 1;
	__builtin_prefetch(bump(), 0, 0);
	__builtin_prefetch(bump(), 1, 3);
	if(calls != 2)
		return 2;

	// unmapped addresses are legal operands: a prefetch never faults
	for(i = 0; i < 64; ++i)
		__builtin_prefetch((void*)(1ul << i), 0, 0);
	__builtin_prefetch((void*)0, 1, 3);
	return 0;
}
