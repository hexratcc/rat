// expect: 0
// output:
//| char 1
//| short 1
//| int 1
//| long 1
//| llong 1
//| bool 1
//| enum 1
//| float 8
//| double 8
//| ldouble 8
//| cdouble 9
//| ptr 5
//| array 5
//| func 5
//| struct 12
//| union 13
//| bitfield 1
#include <stdio.h>

enum E { EA };
struct S {
	int x;
	long double l;
	unsigned k : 12;
};
union U {
	int x;
};
int f(void);

int calls = 0;
int bump(void) {
	++calls;
	return 0;
}

int main(void) {
	char c = 0;
	short s = 0;
	int i = 0;
	long l = 0;
	long long ll = 0;
	_Bool b = 0;
	enum E e = EA;
	float ff = 0;
	double d = 0;
	long double ld = 0;
	_Complex double cd = 0;
	int* p = 0;
	int a[3];
	struct S st;
	union U un;

	printf("char %d\n", __builtin_classify_type(c));
	printf("short %d\n", __builtin_classify_type(s));
	printf("int %d\n", __builtin_classify_type(i));
	printf("long %d\n", __builtin_classify_type(l));
	printf("llong %d\n", __builtin_classify_type(ll));
	printf("bool %d\n", __builtin_classify_type(b));
	printf("enum %d\n", __builtin_classify_type(e));
	printf("float %d\n", __builtin_classify_type(ff));
	printf("double %d\n", __builtin_classify_type(d));
	printf("ldouble %d\n", __builtin_classify_type(ld));
	printf("cdouble %d\n", __builtin_classify_type(cd));
	printf("ptr %d\n", __builtin_classify_type(p));
	printf("array %d\n", __builtin_classify_type(a));
	printf("func %d\n", __builtin_classify_type(f));
	printf("struct %d\n", __builtin_classify_type(st));
	printf("union %d\n", __builtin_classify_type(un));
	printf("bitfield %d\n", __builtin_classify_type(st.k));

	// the operand is a type query, never evaluated
	if(__builtin_classify_type(bump()) != 1)
		return 1;
	if(calls != 0)
		return 2;
	// and it is a constant expression
	{
		static int t[__builtin_classify_type(ld) == 8 ? 1 : -1];
		if(sizeof t != sizeof(int))
			return 3;
	}
	return 0;
}
