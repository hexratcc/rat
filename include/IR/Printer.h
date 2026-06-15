#ifndef RAT_IR_PRINTER_H
#define RAT_IR_PRINTER_H

#include <ostream>

namespace rat {
	struct Function;
	struct Module;

	void print(const Function& fn, std::ostream& os);
	void print(const Module& module, std::ostream& os);
} // namespace rat

#endif
