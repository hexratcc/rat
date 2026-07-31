// expect: 3
// break exits the while loop early once i reaches 3.
int main(void) {
	int i = 0;
	while (i < 100) {
		if (i == 3)
			break;
		i += 1;
	}
	return i;
}
