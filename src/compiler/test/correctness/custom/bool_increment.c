// expect: 0
// ++ on a _Bool sets it to 1
int main(void) {
	_Bool b = 1;
	b++;
	return b != 1;
}
