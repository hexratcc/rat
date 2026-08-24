// expect: 0
// output:
//| char 1 1 1
//| short 2 2 2
//| int 4 4 4
//| long 8 8 8
//| longlong 8 8 8
//| float 4 4 4
//| double 8 8 8
//| longdouble 16 16 16
//| bool 1 1 1
//| ptr 8 8 8
//| enum 4 4 4
//| cfloat 4 8
//| cdouble 8 16
//| cldouble 16 32
//| sc 1 2
//| si 4 8
//| sd 8 16
//| sld 16 32
//| union 8 16
//| arr 4 40
//| arr2 8 64
//| nested 8 24
//| expr 4 8 16

#include <stdalign.h>
#include <stdio.h>

enum E { E0, E1 };
struct SC {
	char a;
	char b;
};
struct SI {
	char a;
	int b;
};
struct SD {
	int a;
	double b;
};
struct SLD {
	char a;
	long double b;
};
union U {
	char c[9];
	double d;
};
struct Nested {
	struct SI a;
	struct SC b;
	double c;
};

int main(void) {
	int i;
	double d;
	long double ld;
	int a[10];
	printf("char %d %d %d\n", (int)_Alignof(char), (int)__alignof__(char), (int)alignof(char));
	printf("short %d %d %d\n", (int)_Alignof(short), (int)__alignof__(short), (int)alignof(short));
	printf("int %d %d %d\n", (int)_Alignof(int), (int)__alignof__(int), (int)alignof(int));
	printf("long %d %d %d\n", (int)_Alignof(long), (int)__alignof__(long), (int)alignof(long));
	printf("longlong %d %d %d\n",
				 (int)_Alignof(long long),
				 (int)__alignof__(long long),
				 (int)alignof(long long));
	printf("float %d %d %d\n", (int)_Alignof(float), (int)__alignof__(float), (int)alignof(float));
	printf(
			"double %d %d %d\n", (int)_Alignof(double), (int)__alignof__(double), (int)alignof(double));
	printf("longdouble %d %d %d\n",
				 (int)_Alignof(long double),
				 (int)__alignof__(long double),
				 (int)alignof(long double));
	printf("bool %d %d %d\n", (int)_Alignof(_Bool), (int)__alignof__(_Bool), (int)alignof(_Bool));
	printf("ptr %d %d %d\n", (int)_Alignof(void *), (int)__alignof__(char *), (int)alignof(int *));
	printf("enum %d %d %d\n", (int)_Alignof(enum E), (int)__alignof__(enum E), (int)alignof(enum E));
	printf("cfloat %d %d\n", (int)_Alignof(float _Complex), (int)sizeof(float _Complex));
	printf("cdouble %d %d\n", (int)_Alignof(double _Complex), (int)sizeof(double _Complex));
	printf(
			"cldouble %d %d\n", (int)_Alignof(long double _Complex), (int)sizeof(long double _Complex));
	printf("sc %d %d\n", (int)_Alignof(struct SC), (int)sizeof(struct SC));
	printf("si %d %d\n", (int)_Alignof(struct SI), (int)sizeof(struct SI));
	printf("sd %d %d\n", (int)_Alignof(struct SD), (int)sizeof(struct SD));
	printf("sld %d %d\n", (int)_Alignof(struct SLD), (int)sizeof(struct SLD));
	printf("union %d %d\n", (int)_Alignof(union U), (int)sizeof(union U));
	printf("arr %d %d\n", (int)_Alignof(int[10]), (int)sizeof(int[10]));
	printf("arr2 %d %d\n", (int)_Alignof(double[8]), (int)sizeof(double[8]));
	printf("nested %d %d\n", (int)_Alignof(struct Nested), (int)sizeof(struct Nested));
	// the GNU form also takes an expression
	printf("expr %d %d %d\n", (int)__alignof__(i), (int)__alignof__(d), (int)__alignof__(ld));
	(void)a;
	return 0;
}
