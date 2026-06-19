// expect: 0
int
main()
{
	struct { int x; } s = { 0 };
	return s.x;
}
