// expect: 290283120
// host-side:
#ifdef RAT_HOST_SIDE
#define SIDE(x) g_##x
#define OTHER(x) r_##x
#else
#define SIDE(x) r_##x
#define OTHER(x) g_##x
#endif
#include "abi_struct.h"

#ifndef RAT_HOST_SIDE
int main(void) { return fold(r_calls() * 3 + g_calls()); }
#endif
