// expect: 9
// Nested loops built from goto/labels count 3*3 iterations.
int main(void) {
	int i = 0;
	int j = 0;
	int count = 0;
outer:
	if (i >= 3) goto end;
	j = 0;
inner:
	if (j >= 3) { i = i + 1; goto outer; }
	count = count + 1;
	j = j + 1;
	goto inner;
end:
	return count;
}
