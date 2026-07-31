// expect: 0
int
main()
{
	int x;

	x = 50;
	while (x)
		x = x - 1;
	return x;
}
