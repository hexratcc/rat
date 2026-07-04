// expect: 0
#include "test.h"

// C99 6.4.4.4: in a preprocessor #if controlling expression, an octal escape in
// a character constant is the backslash followed by one, two, or three octal
// digits. A leading-zero octal escape ('\01', '\012') must use its full value
// (not be short-circuited to 0), and a fourth octal digit ('\1234') is a
// separate character, so '\1234' equals '\123'. The #if branches below set
// each flag only when the preprocessor evaluates the constant correctly.

#if '\01' == 1
#define OCT_LEADING_ZERO_ONE 1
#else
#define OCT_LEADING_ZERO_ONE 0
#endif

#if '\012' == 10
#define OCT_LEADING_ZERO_NL 1
#else
#define OCT_LEADING_ZERO_NL 0
#endif

#if '\0' == 0
#define OCT_NUL 1
#else
#define OCT_NUL 0
#endif

#if '\1234' == '\123'
#define OCT_THREE_DIGIT_CAP 1
#else
#define OCT_THREE_DIGIT_CAP 0
#endif

#if '\377' == 255
#define OCT_MAX_BYTE 1
#else
#define OCT_MAX_BYTE 0
#endif

int main() {
  ASSERT(1, OCT_LEADING_ZERO_ONE);
  ASSERT(1, OCT_LEADING_ZERO_NL);
  ASSERT(1, OCT_NUL);
  ASSERT(1, OCT_THREE_DIGIT_CAP);
  ASSERT(1, OCT_MAX_BYTE);
  printf("OK\n");
  return 0;
}
