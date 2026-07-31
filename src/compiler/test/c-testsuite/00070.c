// expect: 0
#ifndef DEF
int x = 0;
#endif

#define DEF

#ifndef DEF
X
#endif

int
main()
{
	return x;
}
