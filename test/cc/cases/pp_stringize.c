// expect: 1

// `#` stringizes; the XSTR/STR idiom expands its argument first.
#define STR(x) #x
#define XSTR(x) STR(x)
#define N 123

int main(void) {
	const char *a = XSTR(N);
	return a[0] == '1' && a[1] == '2' && a[2] == '3' && a[3] == 0;
}
