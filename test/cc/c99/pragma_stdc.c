// expect: 0
#include "test.h"

// C99 6.10.6 standard pragmas: recognised and ignored (we do not model the
// floating-point control they request). They must not affect compilation.
#pragma STDC FP_CONTRACT ON
#pragma STDC FENV_ACCESS OFF
#pragma STDC CX_LIMITED_RANGE DEFAULT

int main() {
  _Pragma("STDC FP_CONTRACT OFF")
  double a = 2.0, b = 3.0, c = 4.0;
  ASSERT(10, (int)(a * b + c));
  printf("OK\n");
  return 0;
}
