// expect: 26
// A static local array keeps its contents (and its initializer is applied just
// once) across successive calls.
int acc(int x) {
	static int arr[3] = {1, 2, 3};
	static int idx = 0;
	arr[idx] = arr[idx] + x;
	int v = arr[idx];
	idx = (idx + 1) % 3;
	return v;
}
int main(void) {
	return acc(10) + acc(10) + acc(0);
}
