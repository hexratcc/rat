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

	// int sum(int n) {
	//   int s = 0, i = 0;
	//   while (i < n) { s = s + i; i = i + 1; }
	//   return s;
	// }
	// ...ig
	rat::Function* fn = module.createFunction("sum", {i32}, i32);
	rat::Node* n = fn->param(0);
	rat::Function::Var s = fn->declareLocal("s", fn->constInt(i32, 0));
	rat::Function::Var i = fn->declareLocal("i", fn->constInt(i32, 0));
	fn->loop([&] {
		fn->breakIf(fn->sge(fn->get(i), n)); // exit when i >= n
		fn->set(s, fn->add(fn->get(s), fn->get(i)));
		fn->set(i, fn->add(fn->get(i), fn->constInt(i32, 1)));
	});
	fn->ret(fn->get(s));

	rat::PassManager pm;
	pm.add<rat::PrintPass>(std::cout);
	pm.run(module);
	return 0;
}
