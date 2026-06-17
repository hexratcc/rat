#ifndef RAT_IR_TEXTPARSER_H
#define RAT_IR_TEXTPARSER_H

#include "Core.h"

namespace rat {
	struct Module;

	B32 parseText(std::istream& in, Module& module, std::ostream& errors);
	B32 parseText(const String& text, Module& module, std::ostream& errors);
} // namespace rat

#endif
