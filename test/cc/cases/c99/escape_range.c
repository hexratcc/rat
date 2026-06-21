// expect: 0
#include "test.h"

// Numeric (\x and octal) escapes are bounded by the element width of the
// literal (C99 6.4.4.4p9): a narrow char/string element holds 0..0xFF, so a
// value that exactly fills the byte is valid while one that overflows it (e.g.
// '\x1ff' or '\777') is a constraint violation. This confirms the accepted
// boundary; the rejection of an overflowing narrow escape is verified out of
// band since this harness only runs programs that compile.
int main() {
  // narrow byte boundaries: 0xFF fits exactly
  ASSERT(255, (unsigned char)'\xff');
  ASSERT(255, (unsigned char)'\377');
  ASSERT(-1, (char)'\xff');
  ASSERT(-1, (char)'\377');
  ASSERT(0, (unsigned char)'\x00');
  ASSERT(0, (unsigned char)'\0');

  // narrow string elements honor the same range
  ASSERT(255, (unsigned char)"\xff"[0]);
  ASSERT(255, (unsigned char)"\377"[0]);
  ASSERT(126, (unsigned char)"\x7e"[0]);
  ASSERT(255, (unsigned char)"\xfe\xff"[1]);

  printf("OK\n");
  return 0;
}
