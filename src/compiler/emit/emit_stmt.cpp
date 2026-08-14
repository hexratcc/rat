#include "emit/emit.h"

namespace rat::cc {
	Node* Emitter::emitCondPred(Function& fn, const Expr* cond) {
		Value v = emitExpr(fn, cond);
		if(!v.node)
			return nullptr;
		return toBool(fn, v);
	}

	B32 Emitter::emitCondBranch(Function& fn,
															const Expr* e,
															Function::Block* trueB,
															Function::Block* falseB) {
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
		Node* pred = emitCondPred(fn, e);
		if(!pred)
			return false;
		fn.jumpif(pred, trueB);
		fn.jmp(falseB);
		return true;
	}

	B32 Emitter::emitIf(Function& fn, const Stmt* s) {
		Function::Block* thenB = fn.createBlock("if.then");
		Function::Block* elseB = s->elseBody ? fn.createBlock("if.else") : nullptr;
		Function::Block* endB = fn.createBlock("if.end");
		B32 reaches = false;

		if(!emitCondBranch(fn, s->expr, thenB, elseB ? elseB : endB))
			return false;
		if(!elseB)
			reaches = true;

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
		loops.push_back({exitB, header, true});
		B32 ok = emitStmt(fn, s->thenBody);
		loops.pop_back();
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
		loops.push_back({exitB, condB, true});
		B32 ok = emitStmt(fn, s->thenBody);
		loops.pop_back();
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
		pushScope();
		if(s->forInit && !emitStmt(fn, s->forInit)) {
			popScope();
			return false;
		}

		Function::Block* header = fn.createLoopHeader("for.header");
		Function::Block* bodyB = fn.createBlock("for.body");
		Function::Block* postB = fn.createBlock("for.post");
		Function::Block* exitB = fn.createBlock("for.exit");

		fn.jmp(header);
		fn.setInsertBlock(header);
		B32 exitReachable = false;
		if(s->expr) {
			if(!emitCondBranch(fn, s->expr, bodyB, exitB)) {
				popScope();
				return false;
			}
			exitReachable = true;
		} else {
			fn.jmp(bodyB);
		}

		fn.enterBlock(bodyB);
		loops.push_back({exitB, postB, exitReachable});
		B32 ok = emitStmt(fn, s->thenBody);
		LoopFrame frame = loops.back();
		loops.pop_back();
		if(!ok) {
			popScope();
			return false;
		}
		if(!fn.blockFinished())
			fn.jmp(postB);

		fn.enterBlock(postB);
		if(s->forPost) {
			Value post = emitExpr(fn, s->forPost);
			if(!post.node) {
				popScope();
				return false;
			}
		}
		if(!fn.blockFinished())
			fn.jmp(header);

		fn.seal(header);
		fn.seal(exitB);
		if(frame.exitReachable)
			fn.setInsertBlock(exitB);
		popScope();
		return true;
	}

	B32 Emitter::exprRefersTo(const Expr* e, const String& name) const {
		if(!e)
			return false;
		switch(e->kind) {
		case ExprKind::Ident:
			return e->ident.name && *e->ident.name == name;
		case ExprKind::Unary:
			return exprRefersTo(e->unary.operand, name);
		case ExprKind::Binary:
			return exprRefersTo(e->binary.lhs, name) || exprRefersTo(e->binary.rhs, name);
		case ExprKind::Ternary:
			return exprRefersTo(e->ternary.cond, name) || exprRefersTo(e->ternary.whenTrue, name) ||
						 exprRefersTo(e->ternary.whenFalse, name);
		case ExprKind::Comma:
			return exprRefersTo(e->comma.lhs, name) || exprRefersTo(e->comma.rhs, name);
		case ExprKind::Cast:
			return exprRefersTo(e->cast.operand, name);
		case ExprKind::Sizeof:
			return exprRefersTo(e->sizeOf.operand, name);
		case ExprKind::Member:
			return exprRefersTo(e->member.base, name);
		case ExprKind::VaArg:
			return exprRefersTo(e->vaArg.ap, name);
		case ExprKind::CompoundLit:
			if(exprRefersTo(e->compound.init, name))
				return true;
			break;
		case ExprKind::Call:
			if(exprRefersTo(e->call.target, name))
				return true;
			break;
		default:
			break;
		}
		for(const Expr* a : e->args)
			if(exprRefersTo(a, name))
				return true;
		return false;
	}

	void Emitter::collectSwitchCases(const Stmt* s, List<const Stmt*>& cases, const Stmt*& def) {
		if(!s)
			return;
		switch(s->kind) {
		case StmtKind::Case:
			cases.push_back(s);
			return;
		case StmtKind::Default:
			def = s;
			return;
		case StmtKind::Switch:
			return;
		case StmtKind::Compound:
			for(const Stmt* c : s->body)
				collectSwitchCases(c, cases, def);
			return;
		case StmtKind::If:
			collectSwitchCases(s->thenBody, cases, def);
			collectSwitchCases(s->elseBody, cases, def);
			return;
		case StmtKind::While:
		case StmtKind::DoWhile:
		case StmtKind::For:
		case StmtKind::Label:
			collectSwitchCases(s->thenBody, cases, def);
			return;
		default:
			return;
		}
	}

	B32 Emitter::emitSwitch(Function& fn, const Stmt* s) {
		Value ctrl = emitExpr(fn, s->expr);
		if(!ctrl.node)
			return false;
		if(!isInteger(ctrl.type)) {
			fail("switch controlling expression must have integer type");
			return false;
		}
		CType ct = promote(ctrl.type);
		Node* val = convert(fn, ctrl.node, ctrl.type, ct);

		const Stmt* body = s->thenBody;
		if(body->kind != StmtKind::Compound) {
			fail("switch body must be a block");
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
				fail("case label is not an integer constant expression");
				return false;
			}
			for(I64 prev : caseValues) {
				if(prev == v) {
					fail("duplicate case value in switch");
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
		Function::Block* missB = defaultBlock ? defaultBlock : exitB;
		List<U32> order(caseValues.size());
		for(U32 i = 0; i < order.size(); ++i)
			order[i] = i;
		B32 uns = ct.isUnsigned();
		std::sort(order.begin(), order.end(), [&](U32 a, U32 b) {
			if(uns)
				return (U64)caseValues[a] < (U64)caseValues[b];
			return caseValues[a] < caseValues[b];
		});
		// dense case sets dispatch via a jump table: rebase selector to a zero-based
		// slot, range-check, jump indirect; each slot gets a trampoline block (holes
		// -> default) for phi edges, empty ones forwarded away by layout
		U32 n = (U32)order.size();
		// the table's one indirect jump mispredicts badly in hot dispatch loops  so hot
		//  switches stay on the well-predicted compare tree, enclosing-loop is our static hotness proxy
		// (own switch frame is not pushed yet). cold switches still take the table
		B32 inLoop = false;
		for(const LoopFrame& lf : loops)
			if(!lf.isSwitch) {
				inLoop = true;
				break;
			}
		if(!inLoop && n >= 6) {
			I64 minV = caseValues[order[0]];
			I64 maxV = caseValues[order[n - 1]];
			U64 span = (U64)maxV - (U64)minV + 1;
			if(span <= 512 && span <= 4ull * n) {
				Type* selTy = irType(ct);
				Type* i64t = mod.getInt(64);
				Node* idx = fn.binary(Opcode::Sub, val, fn.constInt(selTy, minV));
				Node* idx64 = ct.bits < 64 ? fn.zext(idx, i64t) : idx;
				Function::Block* tableB = fn.createBlock("switch.table");
				fn.jumpif(fn.compare(Opcode::Ult, idx64, fn.constInt(i64t, (I64)span)), tableB);
				fn.jmp(missB);
				fn.enterBlock(tableB);

				List<Function::Block*> slotTarget(span, missB);
				for(U32 i = 0; i < n; ++i)
					slotTarget[(U64)caseValues[order[i]] - (U64)minV] = caseBlocks[order[i]];
				List<Function::Block*> edges;
				edges.reserve(span);
				for(U64 sl = 0; sl < span; ++sl)
					edges.push_back(fn.createBlock("switch.slot"));
				fn.switchJump(idx64, edges);
				for(U64 sl = 0; sl < span; ++sl) {
					fn.enterBlock(edges[sl]);
					fn.jmp(slotTarget[sl]);
				}
				switches.push_back(std::move(blocks));
				loops.push_back({exitB, nullptr, false, true});
				B32 tok = emitStmt(fn, body);
				LoopFrame tframe = loops.back();
				loops.pop_back();
				switches.pop_back();
				if(!tok)
					return false;
				if(!fn.blockFinished()) {
					fn.jmp(exitB);
					tframe.exitReachable = true;
				}
				if(!defaultBlock)
					tframe.exitReachable = true;
				fn.seal(exitB);
				if(tframe.exitReachable)
					fn.setInsertBlock(exitB);
				return true;
			}
		}

		constexpr U32 kLinearMax = 4;
		auto emitRange = [&](auto&& self, U32 lo, U32 hi) -> void {
			if(hi - lo <= kLinearMax) {
				for(U32 i = lo; i < hi; ++i) {
					Node* c = fn.eq(val, fn.constInt(irType(ct), caseValues[order[i]]));
					fn.jumpif(c, caseBlocks[order[i]]);
				}
				fn.jmp(missB);
				return;
			}
			U32 mid = lo + (hi - lo) / 2;
			Function::Block* ltB = fn.createBlock("switch.lt");
			Node* pivot = fn.constInt(irType(ct), caseValues[order[mid]]);
			fn.jumpif(fn.compare(uns ? Opcode::Ult : Opcode::Slt, val, pivot), ltB);
			self(self, mid, hi); // fallthrough side: val >= pivot
			fn.enterBlock(ltB);
			self(self, lo, mid);
		};
		emitRange(emitRange, 0, (U32)order.size());
		switches.push_back(std::move(blocks));
		loops.push_back({exitB, nullptr, false, true});
		B32 ok = emitStmt(fn, body);
		LoopFrame frame = loops.back();
		loops.pop_back();
		switches.pop_back();
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

	B32 Emitter::emitCompound(Function& fn, const Stmt* s) {
		pushScope();
		for(const Stmt* child : s->body) {
			if(fn.blockFinished() && child->kind != StmtKind::Label && child->kind != StmtKind::Case &&
				 child->kind != StmtKind::Default && !containsLabel(child) &&
				 !(!switches.empty() && containsSwitchCase(child))) {
				if(child->kind == StmtKind::Decl && !declareDead(fn, child)) {
					popScope();
					return false;
				}
				continue;
			}
			if(fn.blockFinished() && child->kind != StmtKind::Label && child->kind != StmtKind::Case &&
				 child->kind != StmtKind::Default) {
				Function::Block* dead = fn.createBlock("dead");
				fn.enterBlock(dead);
			}
			if(!emitStmt(fn, child)) {
				popScope();
				return false;
			}
		}
		popScope();
		return true;
	}

	B32 Emitter::emitCaseLabel(Function& fn, const Stmt* s) {
		if(switches.empty()) {
			fail("'case'/'default' label not within a switch");
			return false;
		}
		auto it = switches.back().find(s);
		if(it == switches.back().end()) {
			fail("'case'/'default' label not within a switch");
			return false;
		}
		Function::Block* lbl = it->second;
		if(!fn.blockFinished())
			fn.jmp(lbl);
		fn.enterBlock(lbl);
		return true;
	}

	B32 Emitter::emitStmt(Function& fn, const Stmt* s) {
		curOffset = s->offset;
		switch(s->kind) {
		case StmtKind::Compound:
			return emitCompound(fn, s);
		case StmtKind::Decl:
			return emitDecl(fn, s);
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
			if(loops.empty()) {
				fail("'break' statement not in a loop or switch");
				return false;
			}
			loops.back().exitReachable = true;
			fn.jmp(loops.back().brk);
			return true;
		case StmtKind::Continue: {
			for(auto it = loops.rbegin(); it != loops.rend(); ++it) {
				if(it->isSwitch)
					continue;
				fn.jmp(it->cont);
				return true;
			}
			fail("'continue' statement not in a loop");
			return false;
		}
		case StmtKind::Return:
			return emitReturn(fn, s);
		case StmtKind::Expr:
			return emitExprStmt(fn, s);
		case StmtKind::Empty:
			return true;
		case StmtKind::Label:
			return emitLabel(fn, s);
		case StmtKind::Goto:
			return emitGoto(fn, s);
		}
		fail("unsupported statement");
		return false;
	}

	B32 Emitter::emitReturn(Function& fn, const Stmt* s) {
		if(sretSlot) {
			if(s->expr) {
				Value v = emitExpr(fn, s->expr);
				if(!v.node)
					return false;
				if(isComplexType(curRet)) {
					storeComplex(fn, sretSlot, completeComplex(curRet), v);
				} else if(!isStruct(v.type) || v.type.strukt != curRet.strukt) {
					fail("invalid return value for a struct/union function");
					return false;
				} else {
					emitMemCopy(fn, sretSlot, v.node, curRet.strukt->size);
				}
			}
			fn.ret(sretSlot);
			return true;
		}
		if(isVoidType(curRet)) {
			if(s->expr) {
				Value v = emitExpr(fn, s->expr);
				if(!v.node)
					return false;
				if(!isVoidType(v.type)) {
					fail("return with a value in a function returning void");
					return false;
				}
			}
			fn.retVoid();
			return true;
		}
		Node* value;
		if(s->expr) {
			Value v = emitExpr(fn, s->expr);
			if(!v.node)
				return false;
			value = convert(fn, v.node, v.type, curRet);
		} else {
			value = fn.constInt(irType(curRet), 0);
		}
		fn.ret(value);
		return true;
	}

	B32 Emitter::emitExprStmt(Function& fn, const Stmt* s) {
		Value v = emitExpr(fn, s->expr);
		return v.node != nullptr;
	}

	B32 Emitter::emitLabel(Function& fn, const Stmt* s) {
		auto it = labelBlocks.find(*s->label);
		if(it == labelBlocks.end()) {
			fail("internal: missing block for label '" + *s->label + "'");
			return false;
		}
		Function::Block* lbl = it->second;
		if(!fn.blockFinished())
			fn.jmp(lbl);
		fn.setInsertBlock(lbl);
		return emitStmt(fn, s->thenBody);
	}

	B32 Emitter::emitGoto(Function& fn, const Stmt* s) {
		auto it = labelBlocks.find(*s->label);
		if(it == labelBlocks.end()) {
			fail("use of undeclared label '" + *s->label + "'");
			return false;
		}
		fn.jmp(it->second);
		return true;
	}
} // namespace rat::cc
