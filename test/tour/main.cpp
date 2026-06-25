#include "rat.h"

#include <iostream>

int main() {
	rat::Module module;
	rat::Generic64 target;
	module.setTarget(&target);
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

	// int fold(int x) { return (x + 2) + 3 + (4 * 5) - x * 8; }
	rat::Function* fold = module.createFunction("fold", {i32}, i32);
	rat::Node* x = fold->param(0);
	rat::Node* t = fold->add(fold->add(x, fold->constInt(i32, 2)), fold->constInt(i32, 3));
	t = fold->add(t, fold->mul(fold->constInt(i32, 4), fold->constInt(i32, 5)));
	t = fold->sub(t, fold->mul(x, fold->constInt(i32, 8)));
	fold->ret(t);

	// int gvn(int a, int b) { return (a + b) * (b + a); }
	rat::Function* gvn = module.createFunction("gvn", {i32, i32}, i32);
	rat::Node* a = gvn->param(0);
	rat::Node* b = gvn->param(1);
	gvn->ret(gvn->mul(gvn->add(a, b), gvn->add(b, a)));

	// int cfg(int p, int q) { if (1 < 2) return p; else return q; }
	rat::Function* cfg = module.createFunction("cfg", {i32, i32}, i32);
	rat::Node* p = cfg->param(0);
	rat::Node* q = cfg->param(1);
	rat::Function::Var r = cfg->newVar("r", i32);
	rat::Function::Block* thenB = cfg->createBlock("then");
	rat::Function::Block* merge = cfg->createBlock("merge");
	cfg->jumpif(cfg->slt(cfg->constInt(i32, 1), cfg->constInt(i32, 2)), thenB);
	cfg->set(r, q);
	cfg->jmp(merge);
	cfg->seal(thenB);
	cfg->setInsertBlock(thenB);
	cfg->set(r, p);
	cfg->jmp(merge);
	cfg->seal(merge);
	cfg->setInsertBlock(merge);
	cfg->ret(cfg->get(r));

	// int mem(int* p) { *p = 7; int a = *p; int b = *p; return a + b; }
	rat::Type* ptr = module.getPtr();
	rat::Function* mem = module.createFunction("mem", {ptr}, i32);
	rat::Node* pp = mem->param(0);
	mem->store(pp, mem->constInt(i32, 7));
	rat::Node* la = mem->load(i32, pp);
	rat::Node* lb = mem->load(i32, pp);
	mem->ret(mem->add(la, lb));

	// module storage
	module.createString("greeting", "hi");
	module.createGlobal("counter", i32, /*isConst=*/false, {});

	rat::Function* obj = module.createFunction("obj", {}, i32);
	rat::Node* aSlot = obj->alloc(i32);
	rat::Node* bSlot = obj->alloc(i32);
	obj->store(aSlot, obj->constInt(i32, 1));
	obj->store(bSlot, obj->constInt(i32, 2));
	obj->store(obj->global("counter"), obj->constInt(i32, 5));
	obj->ret(obj->load(i32, aSlot));

	std::cout << "; verify (before opt)\n";
	if (rat::verify(module, std::cout))
		std::cout << "; ok\n";

	std::cout << "\n; before opt\n";
	rat::PassManager before;
	before.add<rat::TextEmitterPass>(std::cout);
	before.run(module);

	std::cout << "\n; after memoryopt + fold + gvn + simplifycfg\n";
	rat::PassManager pm;
	pm.add<rat::MemoryOptPass>();
	pm.add<rat::FoldPass>();
	pm.add<rat::GVNPass>();
	pm.add<rat::SimplifyCFGPass>();
	pm.add<rat::VerifyPass>(std::cout);
	pm.add<rat::TextEmitterPass>(std::cout);
	pm.run(module);

	std::cout << "\n; schedule of sum (blocks in rpo)\n";
	rat::Schedule sched(*fn);
	for (int blockIdx : sched.rpo()) {
		const rat::Schedule::Block& bl = sched.block(blockIdx);
		std::cout << "; block " << blockIdx << " idom=" << bl.idom << " loopDepth=" << bl.loopDepth
							<< " phis=" << bl.phis.size() << " nodes=" << bl.nodes.size() << "\n";
	}

	std::cout << "\n/* --- c --- */\n";
	{
		rat::PassManager cpm;
		cpm.add<rat::CEmitterPass>(std::cout);
		cpm.run(module);
	}

	std::cout << "\n// --- graph ---\n";
	{
		rat::PassManager gpm;
		gpm.add<rat::GraphEmitterPass>(std::cout);
		gpm.run(module);
	}
	return 0;
}
