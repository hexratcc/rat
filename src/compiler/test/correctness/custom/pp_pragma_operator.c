// expect: 9
// C99 6.10.9: the _Pragma operator is converted to a #pragma directive.
// Diagnostic/unknown pragmas are accepted and ignored; the surrounding code
// continues to compile and run normally.
_Pragma("GCC diagnostic push")

#define DO_PRAGMA(x) _Pragma(#x)
DO_PRAGMA(GCC diagnostic ignored "-Wunused")

int main(void) {
	_Pragma("GCC ivdep")
	int s = 0;
	for (int i = 0; i < 9; i++)
		s += 1;
	return s;
}
