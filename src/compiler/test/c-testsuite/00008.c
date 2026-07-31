// expect: 0
int
main()
{
	int x;

	x = 50;
	do 
		x = x - 1;
	while(x);
	return x;
}
