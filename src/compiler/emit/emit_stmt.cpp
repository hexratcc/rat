#include "emit/emit.h"

namespace rat::cc {
	B32 Emitter::emitCondBranch(Function& fn, const Expr* e, Block* trueB, Block* falseB) {
		if(e->kind == ExprKind::Binary &&
			 (e->binary.op == ExprOp::LogAnd || e->binary.op == ExprOp::LogOr)) {
			B32 isAnd = e->binary.op == ExprOp::LogAnd;
			Function::Block* rhsB = fn.createBlock(isAnd ? "and.rhs" : "or.rhs");
			if(!emitCondBranch(fn, e->binary.lhs, isAnd ? rhsB : trueB, isAnd ? falseB : rhsB))
				return false;
			fn.enterBlock(rhsB);
			return emitCondBranch(fn, e->binary.rhs, trueB, falseB);
		}
		if(e->kind == ExprKind::Unary && e->unary.op == ExprOp::Not)
			return emitCondBranch(fn, e->unary.operand, falseB, trueB);
		Value v = emitExpr(fn, e);
		if(!v.node)
			return false;
		fn.jumpif(toBool(fn, v), trueB);
		fn.jmp(falseB);
		return true;
	}

	B32 Emitter::emitIf(Function& fn, const Stmt* s) {
		Function::Block* thenB = fn.createBlock("if.then");
		Function::Block* elseB = s->elseBody ? fn.createBlock("if.else") : nullptr;
		Function::Block* endB = fn.createBlock("if.end");
		B32 reaches = !elseB;

		if(!emitCondBranch(fn, s->expr, thenB, elseB ? elseB : endB))
			return false;

		fn.enterBlock(thenB);
		if(!emitStmt(fn, s->thenBody))
			return false;
		if(!fn.blockFinished()) {
			fn.jmp(endB);
			reaches = true;
		}

		if(elseB) {
			fn.enterBlock(elseB);
			if(!emitStmt(fn, s->elseBody))
				return false;
			if(!fn.blockFinished()) {
				fn.jmp(endB);
				reaches = true;
			}
		}

		fn.seal(endB);
		if(reaches)
			fn.setInsertBlock(endB);
		return true;
	}

	B32 Emitter::emitWhile(Function& fn, const Stmt* s) {
		Function::Block* header = fn.createLoopHeader("while.header");
		Function::Block* bodyB = fn.createBlock("while.body");
		Function::Block* exitB = fn.createBlock("while.exit");

		fn.jmp(header);
		fn.setInsertBlock(header);
		if(!emitCondBranch(fn, s->expr, bodyB, exitB))
			return false;

		fn.enterBlock(bodyB);
		func.loops.push_back({exitB, header, true, false, func.sp});
		B32 ok = emitStmt(fn, s->thenBody);
		func.loops.pop_back();
		if(!ok)
			return false;
		if(!fn.blockFinished())
			fn.jmp(header);

		fn.seal(header);
		fn.enterBlock(exitB);
		return true;
	}

	B32 Emitter::emitDoWhile(Function& fn, const Stmt* s) {
		Function::Block* bodyB = fn.createLoopHeader("do.body");
		Function::Block* condB = fn.createBlock("do.cond");
		Function::Block* exitB = fn.createBlock("do.exit");

		fn.jmp(bodyB);
		fn.setInsertBlock(bodyB);
		func.loops.push_back({exitB, condB, true, false, func.sp});
		B32 ok = emitStmt(fn, s->thenBody);
		func.loops.pop_back();
		if(!ok)
			return false;
		if(!fn.blockFinished())
			fn.jmp(condB);

		fn.enterBlock(condB);
		if(!emitCondBranch(fn, s->expr, bodyB, exitB))
			return false;

		fn.seal(bodyB);
		fn.enterBlock(exitB);
		return true;
	}

	B32 Emitter::emitFor(Function& fn, const Stmt* s) {
		func.scopes.push();
		B32 ok = emitForScoped(fn, s);
		func.scopes.pop();
		return ok;
	}

	B32 Emitter::emitForScoped(Function& fn, const Stmt* s) {
		if(s->forInit && !emitStmt(fn, s->forInit))
			return false;

		Function::Block* header = fn.createLoopHeader("for.header");
		Function::Block* bodyB = fn.createBlock("for.body");
		Function::Block* postB = fn.createBlock("for.post");
		Function::Block* exitB = fn.createBlock("for.exit");

		fn.jmp(header);
		fn.setInsertBlock(header);
		B32 exitReachable = false;
		if(s->expr) {
			if(!emitCondBranch(fn, s->expr, bodyB, exitB))
				return false;
			exitReachable = true;
		} else {
			fn.jmp(bodyB);
		}

		fn.enterBlock(bodyB);
		func.loops.push_back({exitB, postB, exitReachable, false, func.sp});
		B32 ok = emitStmt(fn, s->thenBody);
		LoopFrame frame = func.loops.back();
		func.loops.pop_back();
		if(!ok)
			return false;
		if(!fn.blockFinished())
			fn.jmp(postB);

		fn.enterBlock(postB);
		if(s->forPost && !emitExpr(fn, s->forPost).node)
			return false;
		if(!fn.blockFinished())
			fn.jmp(header);

		fn.seal(header);
		fn.seal(exitB);
		if(frame.exitReachable)
			fn.setInsertBlock(exitB);
		return true;
	}

	B32 Emitter::emitSwitch(Function& fn, const Stmt* s) {
		Value ctrl = emitExpr(fn, s->expr);
		if(!ctrl.node)
			return false;
		if(!isInteger(ctrl.type)) {
			diag.fail("switch controlling expression must have integer type");
			return false;
		}
		CType ct = promote(ctrl.type);
		Node* val = convert(fn, ctrl.node, ctrl.type, ct);

		const Stmt* body = s->thenBody;
		if(body->kind != StmtKind::Compound) {
			diag.fail("switch body must be a block");
			return false;
		}

		Function::Block* exitB = fn.createBlock("switch.exit");
		List<const Stmt*> caseStmts;
		const Stmt* defaultStmt = nullptr;
		collectSwitchCases(body, caseStmts, defaultStmt);

		Map<const Stmt*, Function::Block*> blocks;
		List<I64> caseValues;
		List<Function::Block*> caseBlocks;
		for(const Stmt* c : caseStmts) {
			I64 v;
			if(!evalConst(c->expr, v)) {
				diag.fail("case label is not an integer constant expression");
				return false;
			}
			for(I64 prev : caseValues) {
				if(prev == v) {
					diag.fail("duplicate case value in switch");
					return false;
				}
			}
			Function::Block* b = fn.createBlock("switch.case");
			blocks[c] = b;
			caseValues.push_back(v);
			caseBlocks.push_back(b);
		}
		Function::Block* defaultBlock = nullptr;
		if(defaultStmt) {
			defaultBlock = fn.createBlock("switch.default");
			blocks[defaultStmt] = defaultBlock;
		}

		// dispatch
		CaseSet cs{val, ct, {}, {}, defaultBlock ? defaultBlock : exitB};
		B32 uns = ct.isUnsigned();
		List<Pair<U64, U32>> keyed;
		for(U32 i = 0; i < caseValues.size(); ++i) {
			U64 key = (U64)caseValues[i];
			if(!uns)
				key ^= 1ull << 63;
			keyed.push_back({key, i});
		}
		std::sort(keyed.begin(), keyed.end());
		for(const auto& [key, i] : keyed) {
			cs.values.push_back(caseValues[i]);
			cs.blocks.push_back(caseBlocks[i]);
		}
		// the table's one indirect jump mispredicts badly in hot dispatch loops  so hot
		//  switches stay on the well-predicted compare tree, enclosing-loop is our static hotness proxy
		// (own switch frame is not pushed yet). cold switches still take the table
		B32 inLoop = false;
		for(const LoopFrame& lf : func.loops)
			if(!lf.isSwitch) {
				inLoop = true;
				break;
			}
		if(inLoop || !emitCaseTable(fn, cs))
			emitCaseTree(fn, cs, 0, (U32)cs.values.size());
		func.switches.push_back(std::move(blocks));
		func.loops.push_back({exitB, nullptr, false, true, func.sp});
		B32 ok = emitStmt(fn, body);
		LoopFrame frame = func.loops.back();
		func.loops.pop_back();
		func.switches.pop_back();
		if(!ok)
			return false;

		if(!fn.blockFinished()) {
			fn.jmp(exitB);
			frame.exitReachable = true;
		}
		if(!defaultBlock)
			frame.exitReachable = true;

		fn.seal(exitB);
		if(frame.exitReachable)
			fn.setInsertBlock(exitB);
		return true;
	}

	B32 Emitter::emitCaseTable(Function& fn, const CaseSet& cs) {
		U32 n = (U32)cs.values.size();
		if(n < 6)
			return false;
		I64 minV = cs.values[0];
		U64 span = (U64)cs.values[n - 1] - (U64)minV + 1;
		if(span > 512 || span > 4ull * n)
			return false;
		Type* selTy = irType(cs.ct);
		Type* i64t = mod.getInt(64);
		Node* idx = fn.binary(Opcode::Sub, cs.val, fn.constInt(selTy, minV));
		Node* idx64 = cs.ct.bits < 64 ? fn.zext(idx, i64t) : idx;
		Block* tableB = fn.createBlock("switch.table");
		fn.jumpif(fn.compare(Opcode::Ult, idx64, fn.constInt(i64t, (I64)span)), tableB);
		fn.jmp(cs.miss);
		fn.enterBlock(tableB);

		List<Block*> slotTarget(span, cs.miss);
		for(U32 i = 0; i < n; ++i)
			slotTarget[(U64)cs.values[i] - (U64)minV] = cs.blocks[i];
		List<Block*> edges;
		edges.reserve(span);
		for(U64 sl = 0; sl < span; ++sl)
			edges.push_back(fn.createBlock("switch.slot"));
		fn.switchJump(idx64, edges);
		for(U64 sl = 0; sl < span; ++sl) {
			fn.enterBlock(edges[sl]);
			fn.jmp(slotTarget[sl]);
		}
		return true;
	}

	void Emitter::emitCaseTree(Function& fn, const CaseSet& cs, U32 lo, U32 hi) {
		constexpr U32 kLinearMax = 4;
		if(hi - lo <= kLinearMax) {
			for(U32 i = lo; i < hi; ++i)
				fn.jumpif(fn.eq(cs.val, fn.constInt(irType(cs.ct), cs.values[i])), cs.blocks[i]);
			fn.jmp(cs.miss);
			return;
		}
		U32 mid = lo + (hi - lo) / 2;
		Block* ltB = fn.createBlock("switch.lt");
		Node* pivot = fn.constInt(irType(cs.ct), cs.values[mid]);
		fn.jumpif(fn.compare(cs.ct.isUnsigned() ? Opcode::Ult : Opcode::Slt, cs.val, pivot), ltB);
		emitCaseTree(fn, cs, mid, hi); // fallthrough side: val >= pivot
		fn.enterBlock(ltB);
		emitCaseTree(fn, cs, lo, mid);
	}

	B32 Emitter::declMayBeVla(const Declarator& d) {
		if(isVlaType(d.type))
			return true;
		if(!d.arrayLen)
			return false;
		B32 savedFailed = diag.failed;
		String savedMsg = diag.message;
		I64 count;
		B32 constant = evalConst(d.arrayLen, count);
		diag.failed = savedFailed;
		diag.message = std::move(savedMsg);
		return !constant;
	}

	B32 Emitter::stmtHasVla(const Stmt* s) {
		if(!s)
			return false;
		if(s->kind == StmtKind::Decl) {
			for(const Declarator& d : s->decls) {
				if(d.isStatic)
					continue;
				if(declMayBeVla(d))
					return true;
			}
			return false;
		}
		for(const Stmt* c : s->body)
			if(stmtHasVla(c))
				return true;
		return stmtHasVla(s->thenBody) || stmtHasVla(s->elseBody) || stmtHasVla(s->forInit);
	}

	B32 Emitter::blockDeclaresVla(const Stmt* s) {
		for(const Stmt* c : s->body)
			if(c->kind == StmtKind::Decl && stmtHasVla(c))
				return true;
		return false;
	}

	void Emitter::restoreStack(Function& fn, Node* sp) {
		if(sp && sp != func.sp && !func.sawAlloca && !fn.blockFinished())
			fn.stackRestore(sp);
	}

	B32 Emitter::emitCompound(Function& fn, const Stmt* s) {
		func.scopes.push();
		Node* mark = nullptr;
		Node* outerSp = func.sp;
		if(blockDeclaresVla(s)) {
			mark = fn.stackSave();
			func.sp = mark;
		}
		for(const Stmt* child : s->body) {
			B32 labelLike = child->kind == StmtKind::Label || child->kind == StmtKind::Case ||
											child->kind == StmtKind::Default;
			if(fn.blockFinished() && !labelLike && !containsLabel(child) &&
				 !(!func.switches.empty() && containsSwitchCase(child))) {
				if(child->kind == StmtKind::Decl && !declareDead(fn, child)) {
					func.scopes.pop();
					return false;
				}
				continue;
			}
			if(fn.blockFinished() && !labelLike) {
				Function::Block* dead = fn.createBlock("dead");
				fn.enterBlock(dead);
			}
			if(!emitStmt(fn, child)) {
				func.scopes.pop();
				return false;
			}
		}
		restoreStack(fn, mark);
		func.sp = mark ? mark : outerSp;
		func.scopes.pop();
		return true;
	}

	B32 Emitter::emitCaseLabel(Function& fn, const Stmt* s) {
		Function::Block* lbl = nullptr;
		if(!func.switches.empty()) {
			auto it = func.switches.back().find(s);
			if(it != func.switches.back().end())
				lbl = it->second;
		}
		if(!lbl) {
			diag.fail("'case'/'default' label not within a switch");
			return false;
		}
		if(!fn.blockFinished())
			fn.jmp(lbl);
		fn.enterBlock(lbl);
		return emitStmt(fn, s->thenBody);
	}

	B32 Emitter::emitStmt(Function& fn, const Stmt* s) {
		diag.offset = s->offset;
		switch(s->kind) {
		case StmtKind::Compound:
			return emitCompound(fn, s);
		case StmtKind::Decl:
			for(const Declarator& d : s->decls)
				if(!emitOneDecl(fn, d))
					return false;
			return true;
		case StmtKind::If:
			return emitIf(fn, s);
		case StmtKind::While:
			return emitWhile(fn, s);
		case StmtKind::DoWhile:
			return emitDoWhile(fn, s);
		case StmtKind::For:
			return emitFor(fn, s);
		case StmtKind::Switch:
			return emitSwitch(fn, s);
		case StmtKind::Case:
		case StmtKind::Default:
			return emitCaseLabel(fn, s);
		case StmtKind::Break:
			if(func.loops.empty()) {
				diag.fail("'break' statement not in a loop or switch");
				return false;
			}
			func.loops.back().exitReachable = true;
			restoreStack(fn, func.loops.back().sp);
			fn.jmp(func.loops.back().breakBlock);
			return true;
		case StmtKind::Continue: {
			for(auto it = func.loops.rbegin(); it != func.loops.rend(); ++it) {
				if(it->isSwitch)
					continue;
				restoreStack(fn, it->sp);
				fn.jmp(it->continueBlock);
				return true;
			}
			diag.fail("'continue' statement not in a loop");
			return false;
		}
		case StmtKind::Return:
			return emitReturn(fn, s);
		case StmtKind::Expr:
			return emitExpr(fn, s->expr).node != nullptr;
		case StmtKind::Empty:
			return true;
		case StmtKind::Label:
			return emitLabel(fn, s);
		case StmtKind::Goto:
			return emitGoto(fn, s);
		case StmtKind::Asm:
			return emitAsm(fn, s);
		}
		diag.fail("unsupported statement");
		return false;
	}

	B32 Emitter::emitReturn(Function& fn, const Stmt* s) {
		Value v;
		if(s->expr) {
			v = emitExpr(fn, s->expr);
			if(!v.node)
				return false;
		}
		if(func.sretSlot) {
			if(v.node) {
				if(isComplexType(func.returnType)) {
					storeComplex(fn, func.sretSlot, completeComplex(func.returnType), v);
				} else if(!isStruct(v.type) || v.type.strukt != func.returnType.strukt) {
					diag.fail("invalid return value for a struct/union function");
					return false;
				} else {
					emitMemCopy(fn, func.sretSlot, v.node, func.returnType.strukt->size);
				}
			}
			fn.ret(func.sretSlot);
			return true;
		}
		if(isVoidType(func.returnType)) {
			if(v.node && !isVoidType(v.type)) {
				diag.fail("return with a value in a function returning void");
				return false;
			}
			fn.retVoid();
			return true;
		}
		Node* value;
		if(v.node)
			value = convert(fn, v.node, v.type, func.returnType);
		else
			value = fn.constInt(irType(func.returnType), 0);
		fn.ret(value);
		return true;
	}

	B32 Emitter::emitLabel(Function& fn, const Stmt* s) {
		auto it = func.labelBlocks.find(*s->label);
		if(it == func.labelBlocks.end()) {
			diag.fail("internal: missing block for label '" + *s->label + "'");
			return false;
		}
		Function::Block* lbl = it->second;
		if(!fn.blockFinished())
			fn.jmp(lbl);
		fn.setInsertBlock(lbl);
		func.labelSp[*s->label] = func.sp;
		return emitStmt(fn, s->thenBody);
	}

	B32 Emitter::emitGoto(Function& fn, const Stmt* s) {
		auto it = func.labelBlocks.find(*s->label);
		if(it == func.labelBlocks.end()) {
			diag.fail("use of undeclared label '" + *s->label + "'");
			return false;
		}
		auto sp = func.labelSp.find(*s->label);
		if(sp != func.labelSp.end())
			restoreStack(fn, sp->second);
		fn.jmp(it->second);
		return true;
	}

	static B32 isRegConstraint(const String& c, B32 isOutput, B32& readWrite) {
		U32 i = 0;
		readWrite = false;
		if(isOutput) {
			if(c.empty() || (c[0] != '=' && c[0] != '+'))
				return false;
			readWrite = c[0] == '+';
			i = 1;
		}
		if(i < c.size() && c[i] == '&')
			++i;
		return i + 1 == c.size() && c[i] == 'r';
	}

	static B32 matchingConstraint(const String& c, U32& index) {
		if(c.size() != 1 || c[0] < '0' || c[0] > '9')
			return false;
		index = (U32)(c[0] - '0');
		return true;
	}

	B32 Emitter::emitAsm(Function& fn, const Stmt* s) {
		const AsmBlock* a = s->asmBlock;
		if(a->isGoto) {
			diag.fail("'asm goto' is not supported");
			return false;
		}
		if(!a->text->empty()) {
			diag.fail("inline assembly with a non-empty template is not supported");
			return false;
		}

		List<LValue> lvs;
		List<B32> readsBack(a->outputs.size(), false);
		List<const Expr*> tie(a->outputs.size(), nullptr);
		for(U32 i = 0; i < a->outputs.size(); ++i) {
			const AsmOperand& op = a->outputs[i];
			B32 readWrite = false;
			if(!isRegConstraint(*op.constraint, true, readWrite)) {
				diag.fail("unsupported asm output constraint '" + *op.constraint + "'");
				return false;
			}
			LValue lv;
			if(!emitLValue(fn, op.expr, lv))
				return false;
			if(!asmFitsRegister(lv.type) || lv.isBitfield) {
				diag.fail("asm operand type does not fit a register");
				return false;
			}
			lvs.push_back(lv);
			readsBack[i] = readWrite;
		}

		List<const Expr*> extra; // inputs that no output is tied to
		for(const AsmOperand& op : a->inputs) {
			U32 which = 0;
			if(matchingConstraint(*op.constraint, which)) {
				if(which >= tie.size() || readsBack[which] || tie[which]) {
					diag.fail("asm matching constraint '" + *op.constraint + "' names no free output");
					return false;
				}
				tie[which] = op.expr;
				continue;
			}
			B32 readWrite = false;
			if(!isRegConstraint(*op.constraint, false, readWrite)) {
				diag.fail("unsupported asm input constraint '" + *op.constraint + "'");
				return false;
			}
			extra.push_back(op.expr);
		}
		for(U32 i = 0; i < tie.size(); ++i) {
			if(!tie[i] && !readsBack[i]) {
				diag.fail("asm output operand is not tied to an input, so it has no defined value");
				return false;
			}
		}

		List<Type*> outTypes;
		List<Node*> args;
		for(U32 i = 0; i < lvs.size(); ++i) {
			Node* in = nullptr;
			if(readsBack[i]) {
				in = loadLValue(fn, lvs[i]);
			} else {
				Value v = emitExpr(fn, tie[i]);
				if(!v.node)
					return false;
				in = convert(fn, v.node, v.type, lvs[i].type);
			}
			args.push_back(in);
			outTypes.push_back(irType(lvs[i].type));
		}
		for(const Expr* e : extra) {
			Value v = emitExpr(fn, e);
			if(!v.node)
				return false;
			if(!asmFitsRegister(v.type)) {
				diag.fail("asm operand type does not fit a register");
				return false;
			}
			args.push_back(v.node);
		}

		List<Node*> outs = fn.inlineAsm(*a->text, outTypes, args);
		for(U32 i = 0; i < outs.size(); ++i)
			storeLValue(fn, lvs[i], outs[i]);
		return true;
	}

	B32 Emitter::asmFitsRegister(CType t) const {
		if(isAggregate(t) || isComplexType(t) || isVoidType(t) || isArrayType(t))
			return false;
		return byteSize(t) <= 8;
	}
} // namespace rat::cc
