// expect: 0
#include "test.h"

int main() {
  { int const x; (void)x; }
  { const int x; (void)x; }
  { const int const const x; (void)x; }
  ASSERT(5, ({ const int x = 5; x; }));
  ASSERT(8, ({ const int x = 8; const int *const y=&x; *y; }));
  ASSERT(6, ({ const int x = 6; *(const int * const)&x; }));

  printf("OK\n");
  return 0;
}
