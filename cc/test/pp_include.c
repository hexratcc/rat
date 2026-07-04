// expect: 17

// #include with an include guard; double-include must be a no-op.
#include "pp_inc.h"
#include "pp_inc.h"

int main(void) {
	return helper(10);
}
