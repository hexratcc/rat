// expect: 0
// passes:

int main(void) {
	if(L'\400' != 0400)
		return 1;
	if(L'\x1F600' != 0x1F600)
		return 2;
	if(L"\400"[0] != 0400)
		return 3;
	if(L"\x263A"[0] != 0x263A)
		return 4;
	// a narrow escape still has type char and still sign-extends
	if('\377' != -1)
		return 5;
	return 0;
}
