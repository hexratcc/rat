// expect: 150
// A file-scope array with an initializer list, summed in a loop.
int arr[5] = {10, 20, 30, 40, 50};
int main(void) {
	int s = 0;
	int i = 0;
	for (i = 0; i < 5; i = i + 1)
		s = s + arr[i];
	return s;
}
