// expect: 45
// A common 'goto cleanup' early-exit pattern out of a loop.
int main(void) {
	int acc = 0;
	int i = 0;
	while (i < 100) {
		acc = acc + i;
		if (acc > 40) goto out;
		i = i + 1;
	}
out:
	return acc;
}
