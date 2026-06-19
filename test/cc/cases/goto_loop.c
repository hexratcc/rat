// expect: 10
// A backward goto forms a loop accumulating 0+1+2+3+4.
int main(void) {
	int i = 0;
	int sum = 0;
loop:
	if (i >= 5) goto done;
	sum = sum + i;
	i = i + 1;
	goto loop;
done:
	return sum;
}
