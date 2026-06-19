// expect: 12
// break only exits the innermost loop; outer loop keeps running.
int main(void) {
	int c = 0;
	for (int i = 0; i < 4; i += 1) {
		for (int j = 0; j < 10; j += 1) {
			if (j == 3)
				break;
			c += 1;
		}
	}
	return c;
}
