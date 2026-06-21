// expect: 294
// Sum the byte values of a literal's characters.
int main(void) {
	char *s = "abc";
	int sum = 0;
	int i = 0;
	while (s[i]) {
		sum += s[i];
		i++;
	}
	return sum;
}
