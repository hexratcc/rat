// expect: 0
#include "test.h"

// C99 6.8.6.1: a goto may jump to a label nested inside an otherwise
// unreachable statement. The code between the goto and the label is dead,
// but the label and everything after it remain reachable through the jump.
// Emitting that dead control flow must not corrupt the IR.

int into_if0() {
  int n = 0;
  goto L;
  if (0) {
    L:
    n = 7;
  }
  return n;
}

int into_while() {
  int i = 0, sum = 0;
  goto B;
  while (i < 3) {
    B:
    sum += i;
    i++;
  }
  return sum; // 0+1+2
}

int dead_return() {
  int n = 0;
  goto K;
  if (0) {
    return 99; // dead
  }
  K:
  n = 5;
  return n;
}

int into_switch() {
  int n = 0;
  goto E;
  switch (1) {
  default:
    if (0) {
      E:
      n = 3;
    }
  }
  return n;
}

int forward_skip() {
  int n = 1;
  goto S;
  n = 100; // skipped
  S:
  return n;
}

int main() {
  ASSERT(7, into_if0());
  ASSERT(3, into_while());
  ASSERT(5, dead_return());
  ASSERT(3, into_switch());
  ASSERT(1, forward_skip());

  printf("OK\n");
  return 0;
}
