// expect: 0
#include "test.h"

int main() {
  // __builtin_expect evaluates to its first argument; the hint is advisory.
  ASSERT(1, __builtin_expect(1, 1));
  ASSERT(0, __builtin_expect(0, 1));
  int x = 7;
  ASSERT(1, __builtin_expect(x == 7, 1));

  // __builtin_constant_p is 1 for an integer constant expression, else 0.
  ASSERT(1, __builtin_constant_p(3 + 4));
  ASSERT(0, __builtin_constant_p(x));

  printf("OK\n");
  return 0;
}
