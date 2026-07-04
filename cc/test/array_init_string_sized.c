// expect: 209
// A sized char array initialized from a shorter string literal zero-fills the rest.
// 'h'+'i'+'\0'+'\0' == 209.
int main(void) {
	char s[8] = "hi";
	return s[0] + s[1] + s[2] + s[7];
}
