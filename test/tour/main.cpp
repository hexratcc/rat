#include "IR/Module.h"
#include "IR/Printer.h"

#include <iostream>

int main() {
	rat::Module module;
	rat::Type* i32 = module.getInt(32);

	// int add(int a, int b) { return a + b + 3; }
	rat::Function* add = module.createFunction("add", {i32, i32}, i32);
	rat::Node* sum = add->add(add->param(0), add->param(1));
	add->ret(add->add(sum, add->constInt(i32, 3)));

	rat::print(module, std::cout);
	return 0;
}
