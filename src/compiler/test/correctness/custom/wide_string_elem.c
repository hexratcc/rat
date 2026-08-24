// expect: 0
// passes:

int main(void) {
	if(L"ab"[1] != L'b')
		return 1;
	if(L"a"
		 "b"[1] != L'b')
		return 2;
	if(sizeof(L"ab") != 3 * sizeof(L'a'))
		return 3;
	return 0;
}
