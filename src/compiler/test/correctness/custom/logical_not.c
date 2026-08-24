// expect: 2
// !0 == 1, !5 == 0, !!7 == 1, sum == 2.
int main(void) {
	return !0 + !5 + !!7;
}
