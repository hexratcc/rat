// expect: 0
int foo(void);
int foo(void);
#define FOO 0

int
main()
{
	return FOO;
}
