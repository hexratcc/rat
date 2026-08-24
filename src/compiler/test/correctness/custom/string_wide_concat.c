// expect: 0

int main(void) {
	if(sizeof(u"abc"
						"def") != 14)
		return 1;
	if((u"abc"
			"def")[3] != 'd')
		return 2;
	if(sizeof(U"ab") != 12)
		return 3;
	if((L"abc"
			"def")[3] != 'd')
		return 4;
	if(("\343\201\202" L"")[0] != 0343)
		return 5;
	if(("\343\201\202" L"")[2] != 0202)
		return 6;
	if(L'あ' != 0x3042)
		return 7;
	if(("あ" L"")[0] != 0x3042)
		return 8;
	return 0;
}
