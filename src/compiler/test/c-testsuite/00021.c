// expect: 0
int
foo(int a, int b)
{
	return 2 + a - b;
}

int
main()
{
	return foo(1, 3);
}

