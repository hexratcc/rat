#include "IR/Module.h"
#include "Pass/PassManager.h"
#include "Pass/PrintPass.h"

#include <iostream>

int main() {
	rat::Module module;
	rat::Type* i32 = module.getInt(32);

	// int add(int a, int b) { return a + b + 3; }
	rat::Function* add = module.createFunction("add", {i32, i32}, i32);
	rat::Node* sum = add->add(add->param(0), add->param(1));
	add->ret(add->add(sum, add->constInt(i32, 3)));

	rat::PassManager pm;
	pm.add<rat::PrintPass>(std::cout);
	pm.run(module);
	return 0;
}
